// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef FUXI_BUTTONS_CORE_H
#define FUXI_BUTTONS_CORE_H
#include <stdint.h>
// Independent debounce per key: another held key cannot postpone a release.
struct FuxiButtonsState { uint32_t stable, candidate; uint8_t count[3], held[3], errors; };
static inline uint32_t fuxi_buttons_decode(uint8_t pon,uint8_t gpio) {
 return ((pon&0x80)?2u:0u)|((pon&0x40)?8u:0u)|((gpio&1)?0u:4u);
}
static inline uint32_t fuxi_buttons_sample(FuxiButtonsState *s,uint32_t raw,bool valid,uint32_t latched=0) {
 if(!valid) {
  if(s->errors<3)++s->errors;
  if(s->errors==3){s->stable=0;s->candidate=0;for(unsigned i=0;i<3;++i)s->count[i]=s->held[i]=0;}
  return s->stable;
 }
 s->errors=0;
 raw |= latched & ~s->stable & 0xe;
 for(unsigned i=0;i<3;++i){
  const uint32_t bit=2u<<i;
  if((raw&bit)!=(s->candidate&bit)){s->candidate=(s->candidate&~bit)|(raw&bit);s->count[i]=1;}
  else if(s->count[i]<2)++s->count[i];
  // First observation asserts immediately; debounce only the release.
  // A peripheral edge latch also preserves a tap shorter than one poll.
  if((s->stable&bit) && s->held[i]<30)++s->held[i];
  if(raw&bit){if(!(s->stable&bit)){s->stable|=bit;s->held[i]=0;}}
  else if(s->stable&bit){
   if(s->count[i]>=2 && s->held[i]>=30)s->stable&=~bit;
  }
 }
 return s->stable;
}
#endif
