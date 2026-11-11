// SPDX-License-Identifier: BSD-2-Clause-Patent
// FTS producer -> existing qualified iPadOS17 combined digitizer; one metaclass owner.
#include <libkern/libkern.h>
#include <stdint.h>
#include <stddef.h>
extern "C" {
extern void native_hid_input_probe(void);
extern volatile uint32_t native_hid_stage,native_hid_resource_found,native_hid_input_events[8];
extern volatile uint32_t native_hid_class_sizes[5],native_hid_usrsvc[8];
extern char native_hid_chain[320];
extern volatile int native_hid_load_status,native_hid_matching_load_status,native_hid_input_meta_status,native_hid_input_results[8];
volatile int native_fuxi_touch_enabled;
volatile int native_fuxi_touch_pad=0;
void fuxi_hid_start(void) {
    native_fuxi_touch_enabled=1;
    native_hid_input_probe();
}
bool fuxi_hid_settled(void) {
    return native_hid_stage==3 || native_hid_input_events[2]!=0;
}
const char *fuxi_hid_row(char *out,size_t size) {
    // moduleInitStatus is OSMetaClass::postModLoad()'s return: 0 means registration SUCCEEDED.
    // It is not a class lookup, so it cannot be read as "IOHIDEventService missing".
    snprintf(out,size,"\nFTS HID st%u fam%x evd%x res%u moduleInitStatus%x up%u/%x rep%u/%x fail%u/%x",
        native_hid_stage,(unsigned)native_hid_load_status,(unsigned)native_hid_matching_load_status,
        native_hid_resource_found,(unsigned)native_hid_input_meta_status,
        native_hid_input_events[2],(unsigned)native_hid_input_results[2],
        native_hid_input_events[6],(unsigned)native_hid_input_results[6],
        native_hid_input_events[7],(unsigned)native_hid_input_results[7]);
    size_t used=strlen(out);
    snprintf(out+used,size-used,"\nFTS HID classes UserDevice=%u EventDriver=%u EventService=%u ResourceClient=%u Device=%u",
        native_hid_class_sizes[0],native_hid_class_sizes[1],native_hid_class_sizes[2],
        native_hid_class_sizes[3],native_hid_class_sizes[4]);
    used=strlen(out);
    snprintf(out+used,size-used,"\nFTS HID procs driverkitd=%u AppleUserHID=%u HIDDrivers=%u total=%u chainLen=%u",
        native_hid_usrsvc[0],native_hid_usrsvc[1],native_hid_usrsvc[2],native_hid_usrsvc[3],native_hid_usrsvc[4]);
    used=strlen(out);
    snprintf(out+used,size-used,"\nFTS HID chain %s",native_hid_chain);
    return out;
}
}
