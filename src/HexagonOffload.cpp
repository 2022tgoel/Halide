#include "HexagonOffload.h"
#include "IRMutator.h"
#include "Substitute.h"
#include "Closure.h"
#include "Param.h"
#include "Image.h"
#include "Output.h"
#include "LLVM_Headers.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;

namespace {

Target hexagon_remote_target(Target::HexagonRemote, Target::Hexagon, 32);

}

/////////////////////////////////////////////////////////////////////////////
class InjectHexagonRpc : public IRMutator {
    using IRMutator::visit;

    Module device_code;
    std::map<std::string, int> section_map;
    Expr module_state_ptr;
    Expr module_state_ptr_ptr() {
        return Call::make(Handle(), Call::address_of, {module_state_ptr}, Call::Intrinsic);
    }

    // Create a variable representing the offset to a function with
    // the given name. This remembers the section for substitution
    // after the object is compiled and we can find the actual offset.
    Expr section(const std::string &fn) {
        std::string section = ".text." + fn;
        section_map[section] = -1;
        return Variable::make(type_of<size_t>(), section);
    }

public:
    InjectHexagonRpc() : device_code("hexagon", hexagon_remote_target) {
        Buffer module_state_storage(type_of<void*>(), {}, nullptr, "module_state_ptr");
        *(void **)module_state_storage.host_ptr() = nullptr;
        module_state_ptr = Load::make(type_of<void*>(), "module_state_ptr", 0, module_state_storage, Parameter());
    }

    void visit(const For *loop) {
        if (loop->device_api == DeviceAPI::Hexagon) {
            std::string hex_name = "hex_" + loop->name;

            // Build a closure for the device code.
            // TODO: Should this move the body of the loop to Hexagon,
            // or the loop itself? Currently, this moves the loop itself.
            Closure c(loop);

            // Make an argument list, and generate a function in the
            // device_code module.
            std::vector<Argument> args;
            for (const auto& i : c.buffers) {
                // Output buffers are last (below).
                if (i.second.write) {
                    continue;
                }
                Argument::Kind kind = Argument::InputBuffer;
                args.push_back(Argument(i.first, kind, i.second.type, i.second.dimensions));
            }
            for (const auto& i : c.vars) {
                args.push_back(Argument(i.first, Argument::InputScalar, i.second, 0));
            }
            // Output buffers come last.
            for (const auto& i : c.buffers) {
                if (i.second.write) {
                    Argument::Kind kind = Argument::OutputBuffer;
                    args.push_back(Argument(i.first, kind, i.second.type, i.second.dimensions));
                }
            }
            device_code.append(LoweredFunc(hex_name, args, loop, LoweredFunc::External));

            // Generate a call to hexagon_device_run.
            std::vector<Expr> input_arg_sizes;
            std::vector<Expr> input_arg_ptrs;
            std::vector<Expr> input_arg_flags;
            std::vector<Expr> output_arg_sizes;
            std::vector<Expr> output_arg_ptrs;
            std::vector<Expr> output_arg_flags;

            for (const auto& i : c.buffers) {
                if (i.second.write) continue;

                input_arg_sizes.push_back(Expr((size_t)sizeof(buffer_t*)));
                input_arg_ptrs.push_back(Variable::make(type_of<buffer_t *>(), i.first + ".buffer"));
                input_arg_flags.push_back(1);
            }
            for (const auto& i : c.vars) {
                Expr arg = Variable::make(i.second, i.first);
                Expr arg_ptr = Call::make(type_of<void *>(), Call::make_struct, {arg}, Call::Intrinsic);

                input_arg_sizes.push_back(Expr((size_t)i.second.bytes()));
                input_arg_ptrs.push_back(arg_ptr);
                input_arg_flags.push_back(0);
            }
            for (const auto& i : c.buffers) {
                if (!i.second.write) continue;

                output_arg_sizes.push_back(Expr((size_t)sizeof(buffer_t*)));
                output_arg_ptrs.push_back(Variable::make(type_of<buffer_t *>(), i.first + ".buffer"));
                output_arg_flags.push_back(i.second.read ? 3 : 2);
            }

            // The argument list is terminated with an argument of size 0.
            input_arg_sizes.push_back(Expr((size_t)0));
            output_arg_sizes.push_back(Expr((size_t)0));

            Expr pipeline_argv = section(hex_name + "_argv");

            std::vector<Expr> params;
            params.push_back(module_state_ptr);
            params.push_back(pipeline_argv);
            params.push_back(hex_name);
            params.push_back(Call::make(type_of<size_t*>(), Call::make_struct, input_arg_sizes, Call::Intrinsic));
            params.push_back(Call::make(type_of<void**>(), Call::make_struct, input_arg_ptrs, Call::Intrinsic));
            params.push_back(Call::make(type_of<int*>(), Call::make_struct, input_arg_flags, Call::Intrinsic));
            params.push_back(Call::make(type_of<size_t*>(), Call::make_struct, output_arg_sizes, Call::Intrinsic));
            params.push_back(Call::make(type_of<void**>(), Call::make_struct, output_arg_ptrs, Call::Intrinsic));
            params.push_back(Call::make(type_of<int*>(), Call::make_struct, output_arg_flags, Call::Intrinsic));

            Expr call = Call::make(Int(32), "halide_hexagon_run", params, Call::Extern);
            string call_result_name = unique_name("hexagon_run_result");
            Expr call_result_var = Variable::make(Int(32), call_result_name);
            stmt = LetStmt::make(call_result_name, call,
                                 AssertStmt::make(EQ::make(call_result_var, 0), call_result_var));
        } else {
            IRMutator::visit(loop);
        }
    }

    Stmt inject(Stmt s) {
        s = mutate(s);

        // Skip if there are no device kernels.
        if (device_code.functions.empty()) {
            return s;
        }

        // Compile the device code.
        std::vector<uint8_t> object;
        compile_module_to_object(device_code, object);

        // Put the compiled object into a buffer.
        size_t code_size = object.size();
        Buffer code(type_of<uint8_t>(), {(int)code_size}, nullptr, "code");
        memcpy(code.host_ptr(), &object[0], (int)code_size);

        Expr code_ptr_0 = Load::make(type_of<uint8_t>(), "code", 0, code, Parameter());
        Expr code_ptr = Call::make(Handle(), Call::address_of, {code_ptr_0}, Call::Intrinsic);

        // Wrap the statement in calls to halide_initialize_kernels.
        Expr init_runtime = section("halide_hexagon_init_runtime");
        Expr call = Call::make(Int(32), "halide_hexagon_initialize_kernels",
                               {module_state_ptr_ptr(), code_ptr, Expr(code_size), init_runtime},
                               Call::Extern);
        string call_result_name = unique_name("initialize_kernels_result");
        Expr call_result_var = Variable::make(Int(32), call_result_name);
        s = Block::make(LetStmt::make(call_result_name, call,
                                      AssertStmt::make(EQ::make(call_result_var, 0), call_result_var)),
                        s);

        // Determine the device code offsets from the start of the compiled code.
        llvm::MemoryBufferRef object_ref(llvm::StringRef((char *)object.data(), object.size()), "hexagon_object");
        llvm::ErrorOr<std::unique_ptr<llvm::object::ObjectFile>> obj_file_or_error =
            llvm::object::ObjectFile::createObjectFile(object_ref);
        std::unique_ptr<llvm::object::ObjectFile> obj_file = std::move(*obj_file_or_error);
        internal_assert (!!obj_file)
            << "Failed to open hexagon object file\n";
        for (const llvm::object::SectionRef& i : obj_file->sections()) {
            llvm::StringRef name_ref;
            if (i.getName(name_ref)) continue;
            std::string name(name_ref.data(), name_ref.size());

            // If this section is a function we care about, update the offset.
            std::map<std::string, int>::iterator kernel_offset_i = section_map.find(name);
            if (kernel_offset_i != section_map.end()) {
                internal_assert(kernel_offset_i->second == -1)
                    << "Found duplicate section " << name.data() << ".\n";
                debug(1) << "Found section " << name << " at " << i.getAddress() << ", " << i.getSize() << "\n";
                uint64_t offset = i.getAddress();
                internal_assert(offset < std::numeric_limits<int>::max())
                    << "Offset to function " << name.data() << " is not a 32 bit address\n";
                kernel_offset_i->second = (int)offset;
            }
        }

        // Substitute in the offsets we found.
        std::map<std::string, Expr> substitutions;
        for (const auto& i : section_map) {
            internal_assert(i.second != -1) << "Did not find compiled function " << i.first << ".\n";
            substitutions[i.first] = Expr((size_t)i.second);
        }
        s = substitute(substitutions, s);

        return s;
    }
};

Stmt inject_hexagon_rpc(Stmt s) {
    InjectHexagonRpc injector;
    s = injector.inject(s);
    return s;
}

}  // namespace Internal
}  // namespace Halide
