// Pinned IOBlockStorageDevice imports for fuxi's iOS 17.7 21H16 (iPhone15,2) kernelcache.
//
// These are the original IOStorageFamily addresses of the unchanged MH_FILESET
// kernelcache (sha256 9bd1028796b0baa12922ab44fda1805b11595981338288c7881c669238d352e5).
// XnuPrelink keeps fileset kexts at their link addresses (no --slide), so no slide applies.
// The kext is stripped; each address was recovered and checked against the header
// build/storage-abi/include/IOKit/storage/IOBlockStorageDevice.h:
//   vtable        190 slots; all 22 IOBlockStorageDevice slot discriminators (168..189) match
//                 the header-built vtable, so the slot layout is unchanged.
//   gMetaClass    returned by the vtable's getMetaClass (slot 7).
//   init, getProperty, setProperties, requestIdle, doUnmap, doSetPriority,
//   doGetProvisionStatus
//                 own-kext targets of vtable slots 21, 33, 44, 185, 186, 187, 189.
//   C2            calls IOService::IOService(const OSMetaClass *) and stores this vtable+16
//                 (pacda, discriminator 0xcda1); sits after the MetaClass ctor/dtor pair.
//   D2            BTI entry that tail-calls IOService::~IOService (D2), right after C2.
.globl __ZN20IOBlockStorageDevice10gMetaClassE
.set __ZN20IOBlockStorageDevice10gMetaClassE, 0xfffffff00a485a68

.globl __ZN20IOBlockStorageDevice11requestIdleEv
.set __ZN20IOBlockStorageDevice11requestIdleEv, 0xfffffff009b2da0c

.globl __ZN20IOBlockStorageDevice13doSetPriorityEP26IOBlockStorageDeviceExtentjh
.set __ZN20IOBlockStorageDevice13doSetPriorityEP26IOBlockStorageDeviceExtentjh, 0xfffffff009b2da2c

.globl __ZN20IOBlockStorageDevice13setPropertiesEP8OSObject
.set __ZN20IOBlockStorageDevice13setPropertiesEP8OSObject, 0xfffffff009b2d888

.globl __ZN20IOBlockStorageDevice20doGetProvisionStatusEyyPjP35IOBlockStorageProvisionDeviceExtenty
.set __ZN20IOBlockStorageDevice20doGetProvisionStatusEyyPjP35IOBlockStorageProvisionDeviceExtenty, 0xfffffff009b2da80

.globl __ZN20IOBlockStorageDevice4initEP12OSDictionary
.set __ZN20IOBlockStorageDevice4initEP12OSDictionary, 0xfffffff009b2d6f0

.globl __ZN20IOBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj
.set __ZN20IOBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj, 0xfffffff009b2da1c

.globl __ZN20IOBlockStorageDeviceC2EPK11OSMetaClass
.set __ZN20IOBlockStorageDeviceC2EPK11OSMetaClass, 0xfffffff009b2d614

.globl __ZN20IOBlockStorageDeviceD2Ev
.set __ZN20IOBlockStorageDeviceD2Ev, 0xfffffff009b2d648

.globl __ZNK20IOBlockStorageDevice11getPropertyEPK8OSSymbol
.set __ZNK20IOBlockStorageDevice11getPropertyEPK8OSSymbol, 0xfffffff009b2d780

.globl __ZTV20IOBlockStorageDevice
.set __ZTV20IOBlockStorageDevice, 0xfffffff007c79ef8
