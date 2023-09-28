#include <hpx/config.hpp>
#include "hpxComms.hpp"
#include <memory>

#if !defined(HPX_COMPUTE_DEVICE_CODE)

namespace SST { namespace Comms { namespace Component {

void P2PCommunicator::invoke(const std::string ibuf) {
    std::lock_guard<hpx::mutex> lg(mut);
    data = ibuf;
}

} /* namespace Component */ } /* namespace Comms */ } // namespace SST

HPX_REGISTER_COMPONENT_MODULE()

using comms_component_type = hpx::components::component<SST::Comms::Component::P2PCommunicator>;

HPX_REGISTER_COMPONENT(comms_component_type, P2PCommunicator)

HPX_REGISTER_ACTION(
    SST::Comms::Component::P2PCommunicator::invoke_action, comms_component_invoke_action)

#endif
