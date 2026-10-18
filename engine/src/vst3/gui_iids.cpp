// Interface identifiers for the VST3 editor interfaces this host implements or
// queries (plug-in view, frame and the Linux run loop). FUnknown's own id lives
// in coreiids.cpp; these are defined once here from the header's constants.
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/gui/iplugview.h>

namespace Steinberg {
DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)
namespace Linux {
DEF_CLASS_IID(IEventHandler)
DEF_CLASS_IID(ITimerHandler)
DEF_CLASS_IID(IRunLoop)
} // namespace Linux
} // namespace Steinberg
