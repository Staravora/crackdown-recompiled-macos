#include "thermal_state.h"

#import <Foundation/Foundation.h>

#include <rex/logging.h>

namespace crackdown::native {

const char* ThermalState() {
  switch ([NSProcessInfo processInfo].thermalState) {
    case NSProcessInfoThermalStateNominal:
      return "nominal";
    case NSProcessInfoThermalStateFair:
      return "fair";
    case NSProcessInfoThermalStateSerious:
      return "serious";
    case NSProcessInfoThermalStateCritical:
      return "critical";
  }
  return "unknown";
}

void WatchThermalState() {
  static bool watching = false;
  if (watching) {
    return;
  }
  watching = true;
  REXLOG_INFO("Thermal state: {}", ThermalState());
  [[NSNotificationCenter defaultCenter] addObserverForName:NSProcessInfoThermalStateDidChangeNotification
                                                    object:nil
                                                     queue:nil
                                                usingBlock:^(NSNotification*) {
                                                  REXLOG_INFO("Thermal state: {}", ThermalState());
                                                }];
}

}  // namespace crackdown::native
