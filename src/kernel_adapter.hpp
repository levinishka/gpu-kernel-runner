#ifndef KERNEL_ADAPTER_HPP_
#define KERNEL_ADAPTER_HPP_

#include "execution_context.hpp"
#include "parsers.hpp"

#include <util/miscellany.hpp>
#include <util/functional.hpp>
#include <util/factory_producible.hpp>
#include <util/optional_and_any.hpp>
#include <util/static_block.hpp>
    // This file itself does not use static blocks, but individual kernel adapters may
    // want to use them for registering themselves in the factory.

#include <cxxopts/cxxopts.hpp>
    // This is necessary, since the adapter injects command-line options
    // which are specific to the kernel, that can be displayed and parsed
    // by the generic kernel runner

#include <cxx-prettyprint/prettyprint.hpp>

#include <common_types.hpp>

// A convenience overload for specific kernel adapters to be able
// to complain about dimensions_t's they get.
inline std::ostream& operator<<(std::ostream& os, cuda::grid::dimensions_t dims)
{
    return os << '(' << dims.x << " x " << dims.y << " x " << dims.z << " x " << ')';
}

using size_calculator_type = std::size_t (*)(
    const host_buffers_map& input_buffers,
    const scalar_arguments_map& scalar_arguments,
    const preprocessor_definitions_t& valueless_preprocessor_definitions,
    const preprocessor_value_definitions_t& value_preprocessor_definitions);

static constexpr const size_calculator_type no_size_calc = nullptr;


namespace kernel_adapters {

using key = std::string;
    // I have also considered, but rejected for now, the option of
    // struct key { string variant; string name; };

} //namespace kernel_adapters

/**
 * This class (or rather, its concrete subclasses) encapsulates
 * all the apriori information and logic specifically regarding a single
 * kernel - and hopefully nothing else. The rest of the kernel runner's
 * code knows nothing about any specific kernel, and uses the methods here
 * to obtain this information, uniformly for all kernels. Any logic
 * _not_ dependent on the kernel should not be in this class - not in
 * the child classes, but not even in this abstract base class.
 *
 * @note This class' methods...
 *
 *   - Do not allocate, de-allocate or own any large buffers
 *   - Do not perform any significant computation
 *   - Do not trigger memory copy to/from CUDA devices, nor kernel execution
 *     on CUDA devices, etc.
 *   - May make CUDA API calls to determine information about CUDA devices.
 */
class kernel_adapter : util::mixins::factory_producible<kernel_adapters::key, kernel_adapter> {
public:
    using key_type = kernel_adapters::key;
    using mixin_type = util::mixins::factory_producible<key_type, kernel_adapter>;
    using mixin_type::can_produce_subclass;
    using mixin_type::produce_subclass;
    using mixin_type::get_subclass_factory;
    using mixin_type::register_in_factory;

protected:
    // This will make it easier for subclasses to implement the parameter_details function
    static constexpr const auto input  = parameter_direction_t::input;
    static constexpr const auto output = parameter_direction_t::output;
    static constexpr const auto buffer = kernel_parameters::kind_t::buffer;
    static constexpr const auto scalar = kernel_parameters::kind_t::scalar;
    static constexpr const auto inout  = parameter_direction_t::inout;
    static constexpr const bool is_required = kernel_parameters::is_required;
    static constexpr const bool isnt_required = kernel_parameters::isnt_required;

public: // constructors & destructor
    kernel_adapter() = default;
    kernel_adapter(const kernel_adapter&) = default;
    virtual ~kernel_adapter() = default;
    kernel_adapter& operator=(kernel_adapter&) = default;
    kernel_adapter& operator=(kernel_adapter&&) = default;


    struct single_parameter_details {
        const char* name;
        kernel_parameters::kind_t kind;
        parser_type parser;
        size_calculator_type size_calculator;
        parameter_direction_t direction; // always in for scalars
        bool required;
        const char* description;
    };

    struct single_preprocessor_definition_details {
        const char* name;
        const char* description;
        bool required;
    };

    // TODO: This should really be a span (and then we wouldn't
    // need to use const-refs to it)
    using parameter_details_type = std::vector<single_parameter_details>;
    using preprocessor_definitions_type = std::vector<single_preprocessor_definition_details>;

public:
    /**
     * @brief The key for each adapter has multiple uses: It's used to look it up dynamically
     * and create an instance of it; it's used as a default path suffix for the kernel file;
     * it's used to identify which kernel is being run, to the user; it may be used for output
     * file generation; etc.
     */
    virtual std::string key() const = 0;

    // Q: Why is the value of this function not the same as the key?
    // A: Because multiple variants of the same kernel may use the same kernel function name,
    // e.g. in CUDA and in OpenCL, with different kinds of optimizations etc.
    virtual std::string kernel_function_name() const = 0;

    // Note: Inheriting classes must define a key_type key_ static member -
    // or else they cannot be registered in the factory.

    virtual const parameter_details_type & parameter_details() const = 0;
    virtual parameter_details_type scalar_parameter_details() const
    {
        parameter_details_type all_params = parameter_details();
        return util::filter(all_params, [](const single_parameter_details& param) { return param.kind == scalar; });
    }
    virtual parameter_details_type buffer_details() const
    {
        parameter_details_type all_params = parameter_details();
        return util::filter(all_params, [](const single_parameter_details& param) { return param.kind == buffer; });
    }
    virtual const preprocessor_definitions_type& preprocessor_definition_details() const = 0;


    virtual void add_buffer_cmdline_options(cxxopts::OptionAdder adder) const
    {
        for(const auto& buffer_ : buffer_details() ) {
            adder(buffer_.name, buffer_.description,  cxxopts::value<std::string>()->default_value(buffer_.name));
        }
    }

    virtual void add_scalar_arguments_cmdline_options(cxxopts::OptionAdder option_adder) const {
        for(const auto& sad : scalar_parameter_details()) {
            option_adder(sad.name, sad.description, cxxopts::value<std::string>());
        }
    }

    virtual void add_preprocessor_definition_cmdline_options(cxxopts::OptionAdder option_adder) const {
        for(const auto& pd : preprocessor_definition_details()) {
            option_adder(pd.name, pd.description, cxxopts::value<std::string>());
        }
    }

protected:
    static parameter_name_set buffer_names_from_details(const parameter_details_type& param_details)
    {
        parameter_name_set names;
        std::transform(
            param_details.cbegin(), param_details.cend(),
            std::inserter(names, names.begin()),
            [](const single_parameter_details& details) { return details.name; }
        );
        return names;
    }

public:
    virtual parameter_name_set buffer_names(parameter_direction_t direction) const
    {
        auto& all_params = parameter_details();
        auto requested_dir_buffers_only =
            util::filter(all_params,
                [&direction](const single_parameter_details& spd) {
                    return spd.direction == direction and spd.kind == buffer;
                }
            );
        return buffer_names_from_details(requested_dir_buffers_only);
    }

    virtual any parse_cmdline_scalar_argument(
        const single_parameter_details& parameter_details,
        const std::string& value_str) const
    {
        return parameter_details.parser(value_str);
    }

    virtual scalar_arguments_map generate_additional_scalar_arguments(execution_context_t&) const { return {}; }

    // Try not to require the whole context

    virtual bool extra_validity_checks(const execution_context_t&) const { return true; }
    virtual bool input_sizes_are_valid(const execution_context_t&) const { return true; }

protected:
    /**
     * Same as @ref `marshal_kernel_arguments()`, but not required to be terminated with a `nullptr`;
     */
    virtual void marshal_kernel_arguments_inner(
        marshalled_arguments_type& arguments,
        const execution_context_t& context) const = 0;
public:

    /**
     * Marshals an array of pointers which can be used for a CUDA/OpenCL-driver-runnable kernel's
     * arguments.
     *
     * @param context A fully-populated test execution context, containing all relevant buffers
     * and scalar arguments.
     * @return the marshaled array of pointers, which may be passed to cuLaunchKernel or
     * clEnqueueNDRangeKernel. For CUDA, it is nullptr-terminated; for OpenCL, we also fill
     * an array of argument sizes.
     *
     * @note This method will get invoked after we've already used the preprocessor
     * definitions to compile the kernels. It may therefore assume they are all present and valid
     * (well, valid enough to compile).
     *
     * @TODO I think we can probably arrange it so that the specific adapters only need to specify
     * the sequence of names, and this function can take care of all the rest - seeing how
     * launching gets the arguments in a type-erased fashion.
     *
     */
    marshalled_arguments_type marshal_kernel_arguments(const execution_context_t& context) const
    {
        marshalled_arguments_type argument_ptrs_and_maybe_sizes;
        marshal_kernel_arguments_inner(argument_ptrs_and_maybe_sizes, context);
        if (context.ecosystem == execution_ecosystem_t::cuda) {
            argument_ptrs_and_maybe_sizes.pointers.push_back(nullptr);
                // cuLaunchKernels uses a termination by NULL rather than a length parameter.
                // Note: Remember that sizes is unused in this case
        }
        return argument_ptrs_and_maybe_sizes;
    }

    virtual optional_launch_config_components deduce_launch_config(const execution_context_t& context) const
    {
        auto components = context.options.forced_launch_config_components;
        if (not components.dynamic_shared_memory_size) {
            components.dynamic_shared_memory_size = 0;
        }
        if (components.is_sufficient()) {
            return components;
        }

        throw std::runtime_error(
            "Unable to deduce launch configuration - please specify all launch configuration components "
            "explicitly using the command-line");
    }

    optional_launch_config_components make_launch_config(const execution_context_t& context) const {
        auto& forced = context.options.forced_launch_config_components;
        if (forced.is_sufficient()) {
            return forced;
        }
        else return deduce_launch_config(context);
    }
};

namespace kernel_adapters {

template <typename U>
static void register_in_factory()
{
    bool dont_ignore_repeat_registrations { false };
    kernel_adapter::register_in_factory<U>(U::key_, dont_ignore_repeat_registrations);
}

// TODO:
// 1. Perhaps we should wrap the raw argument vector with methods for pushing back?
//    and arrange it so that when its used, e.g. for casting into a void**, we also
//    append the final nullptr?
// 2. Consider placing the argument_ptrs vector in the test context; not sure why
//    it should be outside of it.
inline void push_back_buffer(
    marshalled_arguments_type& argument_ptrs,
    const execution_context_t& context,
    parameter_direction_t dir,
    const char* buffer_parameter_name)
{
    const auto& buffer_map = (dir == parameter_direction_t::in) ?
        context.buffers.device_side.inputs:
        context.buffers.device_side.outputs;
        // Note: We use outputs here for inout buffers as well.
    if (context.ecosystem == execution_ecosystem_t::cuda) {
        argument_ptrs.pointers.push_back(& buffer_map.at(buffer_parameter_name).cuda.data());
    }
    else {
        argument_ptrs.pointers.push_back(& buffer_map.at(buffer_parameter_name).opencl);
        argument_ptrs.sizes.push_back(sizeof(cl::Buffer));
    }
}

template <typename Scalar>
inline void push_back_scalar(
    marshalled_arguments_type& argument_ptrs,
    const execution_context_t& context,
    const char* scalar_parameter_name)
{
    argument_ptrs.pointers.push_back(& any_cast<const Scalar&>(context.scalar_input_arguments.typed.at(scalar_parameter_name)));
    if (context.ecosystem == execution_ecosystem_t::opencl) {
        argument_ptrs.sizes.push_back(sizeof(Scalar));
    }
}

} // namespace kernel_adapters

inline parameter_name_set buffer_names(const kernel_adapter& adapter, parameter_direction_t dir_1, parameter_direction_t dir_2)
{
    return util::union_(adapter.buffer_names(dir_1), adapter.buffer_names(dir_2));
}

// Boilerplate macros for subclasses of kernel_adapter.
// Each of these needs to be invoked once in any subclass
// definition

// The name of the kernel function in the source file
#define KA_KERNEL_FUNCTION_NAME(kfn) \
    constexpr static const char* kernel_function_name_ { kfn }; \
    std::string kernel_function_name() const override { return kernel_function_name_; }

// The key to be passed to the kernel-runner executable for using
// this kernel adapter. Must be unique.
#define KA_KERNEL_KEY(kk) \
    constexpr static const char* key_ { kk }; \
    std::string key() const override { return key_; }

// Use this macro to succinctly generate a "size calculator" function
// which returns the size of one of the input parameters. If your
// size calculation is more complex, just implement your own size
// calculator

#define KA_SIZE_CALCULATOR_BY_INPUT_BUFFER(function_name, input_buffer) \
    std::size_t function_name ( \
        const host_buffers_map& input_buffers, \
        const scalar_arguments_map&, \
        const preprocessor_definitions_t&, \
        const preprocessor_value_definitions_t&) \
    { \
        return input_buffers.at(#input_buffer).size(); \
    }

#endif /* KERNEL_ADAPTER_HPP_ */
