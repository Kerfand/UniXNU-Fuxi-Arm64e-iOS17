// Generated for iOS 17.7 (21H16) AppleUSBXDCI (vtable 0xfffffff007c98748, 267 slots, size 0x2440)
#pragma once
#include <IOKit/IOService.h>
#define XDCI_KEEP_CONFIGURATIONS opaque179_26116
#define XDCI_TRANSPORT_ACTIVE opaque255_1456
#define XDCI_POWER_ON opaque256_66040
#define XDCI_POWER_OFF opaque257_146012
#define XDCI_PARAMETERS opaque258_254344
#define XDCI_CONTROLLER_RESET opaque263_26570
class AppleUSBXDCI : public IOService {
 OSDeclareAbstractStructors(AppleUSBXDCI)
 unsigned char originalPrivate[0x2440 - sizeof(IOService)];
public:
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool terminate(IOOptionBits options=0) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
 unsigned long maxCapabilityForDomainState(IOPMPowerFlags) override;
 unsigned long initialPowerStateForDomainState(IOPMPowerFlags) override;
 IOReturn powerStateWillChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;
 IOReturn powerStateDidChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;
 virtual void opaque168_102112();
 virtual void opaque169_152503();
 virtual void opaque170_53512();
 virtual void opaque171_7951();
 virtual void opaque172_4040();
 virtual void opaque173_93847();
 virtual void opaque174_7525();
 virtual void opaque175_49064();
 virtual void opaque176_66399();
 virtual void opaque177_25823();
 virtual void opaque178_101457();
 virtual bool opaque179_26116();
 virtual void opaque180_10290();
 virtual void opaque181_107862();
 virtual void opaque182_72724();
 virtual void opaque183_19523();
 virtual void opaque184_102883();
 virtual void opaque185_203034();
 virtual void opaque186_8774();
 virtual void opaque187_13958();
 virtual void opaque188_112849();
 virtual void opaque189_9167();
 virtual void opaque190_57742();
 virtual void opaque191_209627();
 virtual void opaque192_52536();
 virtual void opaque193_85686();
 virtual void opaque194_18159();
 virtual void opaque195_63203();
 virtual void opaque196_9200();
 virtual void opaque197_12096();
 virtual void opaque198_109252();
 virtual void opaque199_62712();
 virtual void opaque200_46403();
 virtual void opaque201_67402();
 virtual void opaque202_39281();
 virtual void opaque203_46807();
 virtual void opaque204_7818();
 virtual void opaque205_61404();
 virtual void opaque206_7551();
 virtual void opaque207_96801();
 virtual void opaque208_21278();
 virtual void opaque209_38750();
 virtual void opaque210_19030();
 virtual void opaque211_107905();
 virtual void opaque212_72553();
 virtual void opaque213_120602();
 virtual void opaque214_150369();
 virtual void opaque215_174061();
 virtual void opaque216_52130();
 virtual void opaque217_189826();
 virtual void opaque218_44954();
 virtual void opaque219_48982();
 virtual void opaque220_141045();
 virtual void opaque221_202094();
 virtual void opaque222_203584();
 virtual void opaque223_17575();
 virtual void opaque224_119234();
 virtual void opaque225_1457();
 virtual void opaque226_51882();
 virtual void opaque227_175308();
 virtual void opaque228_45920();
 virtual void opaque229_104551();
 virtual void opaque230_64211();
 virtual void opaque231_15115();
 virtual void opaque232_61666();
 virtual void opaque233_6570();
 virtual void opaque234_5516();
 virtual void opaque235_48514();
 virtual void opaque236_37106();
 virtual void opaque237_170440();
 virtual void opaque238_63705();
 virtual void opaque239_160419();
 virtual void opaque240_145188();
 virtual void opaque241_216034();
 virtual void opaque242_102494();
 virtual void opaque243_24704();
 virtual void opaque244_6457();
 virtual void opaque245_87010();
 virtual void opaque246_21640();
 virtual void opaque247_38658();
 virtual void opaque248_7466();
 virtual void opaque249_4474();
 virtual void opaque250_8168();
 virtual void opaque251_268655();
 virtual void opaque252_64375();
 virtual void opaque253_60646();
 virtual void opaque254_30020();
 virtual bool opaque255_1456();
 virtual IOReturn opaque256_66040();
 virtual IOReturn opaque257_146012();
 virtual void opaque258_254344();
 virtual void opaque259_107685();
 virtual void opaque260_10898();
 virtual void opaque261_230959();
 virtual void opaque262_18411();
 virtual IOReturn opaque263_26570();
 virtual void opaque264_74597();
 virtual void opaque265_27978();
 virtual void opaque266_29674();
};
static_assert(sizeof(AppleUSBXDCI) == 0x2440, "iOS 17.7 AppleUSBXDCI size");
