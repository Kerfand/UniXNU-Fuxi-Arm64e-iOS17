"""logical.py (2026-11-11): NativeLegacyDisplay reports a logical mode larger than the GOP panel and scales every frame down on the CPU.
boot-args offline-legacy-logical-width / -height (fuxi: 1179x2556 = iPhone 14 Pro 393x852 pt at 3x, the ADT identity's artwork size;
horizontally Xiaomi 13's official 440 dpi = 2.75x). Without them nothing changes. Scaled frames always take the CPU copy (no direct flip,
no GPU offer): bilinear, each source row converted to 8-bit BGRA once, column/row maps allocated on first use (no static __DATA)."""
from pathlib import Path
f = Path('/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/drivers/GopFramebuffer/NativeLegacyDisplay.cpp')
t = f.read_text()
if 'offline-legacy-logical-width' in t:
    print('already'); raise SystemExit


def rep(old, new, count=1):
    global t
    assert t.count(old) == count, (t.count(old), old[:90])
    t = t.replace(old, new)


helpers = r'''
// offline-legacy-logical-width / offline-legacy-logical-height: the display reports a logical mode at least as large as the panel (fuxi:
// 1179x2556, iPhone 14 Pro's 393x852 pt at 3x, on a 1080x2400 panel) and every frame is scaled down to the GOP on the CPU.
static uint32_t gLogicalW,gLogicalH,gPhysW,gPhysH;
static bool gScaled;
static void logicalConfigure(uint32_t physW,uint32_t physH) {
    uint32_t w=0,h=0;
    PE_parse_boot_argn("offline-legacy-logical-width",&w,sizeof(w));
    PE_parse_boot_argn("offline-legacy-logical-height",&h,sizeof(h));
    gPhysW=physW;gPhysH=physH;
    gScaled=w>=physW && h>=physH && (w!=physW || h!=physH) && w<=8192 && h<=8192;
    gLogicalW=gScaled?w:physW;gLogicalH=gScaled?h:physH;
}
// The display's (logical) size against the GOP the frame goes to.
static bool scanoutMatches(uint32_t w,uint32_t h,const PE_Video &v) {
    return gScaled?(w==gLogicalW && h==gLogicalH && v.v_width==gPhysW && v.v_height==gPhysH):(w==v.v_width && h==v.v_height);
}
static const uint8_t *xrTable() {
    static uint8_t table[1024];static bool made;
    if(!made) {
        for(unsigned v=0;v<1024;++v)table[v]=v<=384?0:(v>=894?255:uint8_t(((v-384)*255+255)/510));
        made=true;
    }
    return table;
}
enum ScaleSource {kScaleBGRA,kScaleXr,kScaleHalf};
// One source row to 8-bit BGRA words.
static bool scaleConvertRow(ScaleSource kind,const uint8_t *row,uint32_t *out,uint32_t width) {
    if(kind==kScaleXr) {
        const uint8_t *table=xrTable();const uint32_t *in=reinterpret_cast<const uint32_t *>(row);
        for(uint32_t x=0;x<width;++x){const uint32_t p=in[x];out[x]=(uint32_t(table[(p>>20)&0x3ff])<<16)|(uint32_t(table[(p>>10)&0x3ff])<<8)|table[p&0x3ff];}
    } else if(kind==kScaleHalf) {
        const uint8_t *table=halfTable();if(!table)return false;const uint64_t *in=reinterpret_cast<const uint64_t *>(row);
        for(uint32_t x=0;x<width;++x) {
            const uint64_t p=in[x];
            const uint32_t r=uint32_t(p)&0xffff,g=uint32_t(p>>16)&0xffff,b=uint32_t(p>>32)&0xffff;
            out[x]=(uint32_t((r&0x8000)?0:table[r])<<16)|(uint32_t((g&0x8000)?0:table[g])<<8)|((b&0x8000)?0:table[b]);
        }
    } else {
        const uint32_t *in=reinterpret_cast<const uint32_t *>(row);
        for(uint32_t x=0;x<width;++x)out[x]=in[x];
    }
    return true;
}
// Bilinear sw x sh -> dw x dh (dw <= sw, dh <= sh), opaque BGRA out. Returns false when a row runs past either end.
static bool scaleRows(volatile void *dst,uint64_t dstLength,uint64_t dstStride,uint32_t dw,uint32_t dh,
                      const void *src,uint64_t srcLength,uint64_t srcStride,uint32_t sw,uint32_t sh,ScaleSource kind) {
    const uint32_t bpp=kind==kScaleHalf?8:4;
    if(!dw || !dh || !sw || !sh || dstStride<uint64_t(dw)*4 || srcStride<uint64_t(sw)*bpp ||
       dstStride*(dh-1)+uint64_t(dw)*4>dstLength || srcStride*(sh-1)+uint64_t(sw)*bpp>srcLength)return false;
    static uint32_t *xmap,*rows;static uint32_t xmapW,xmapSW,rowsW;
    if(xmapW!=dw || xmapSW!=sw) {
        if(xmap)IOFree(xmap,uint64_t(xmapW)*4);
        xmap=static_cast<uint32_t *>(IOMalloc(uint64_t(dw)*4));xmapW=xmap?dw:0;xmapSW=xmap?sw:0;
        if(!xmap)return false;
        for(uint32_t x=0;x<dw;++x) {
            int64_t f=(int64_t(2*x+1)*sw*256)/(int64_t(2)*dw)-128;if(f<0)f=0;
            uint32_t i=uint32_t(f>>8),w=uint32_t(f&255);if(i>=sw-1){i=sw-1;w=0;}
            xmap[x]=(i<<8)|w;
        }
    }
    if(rowsW<sw) {
        if(rows)IOFree(rows,uint64_t(rowsW)*8);
        rows=static_cast<uint32_t *>(IOMalloc(uint64_t(sw)*8));rowsW=rows?sw:0;
        if(!rows)return false;
    }
    uint32_t *r0=rows,*r1=rows+sw;int64_t have0=-1,have1=-1;
    for(uint32_t y=0;y<dh;++y) {
        int64_t f=(int64_t(2*y+1)*sh*256)/(int64_t(2)*dh)-128;if(f<0)f=0;
        uint32_t i=uint32_t(f>>8),wy=uint32_t(f&255),j=i+1;if(i>=sh-1){i=sh-1;j=i;wy=0;}
        if(have0!=int64_t(i)) {
            if(have1==int64_t(i)){uint32_t *s=r0;r0=r1;r1=s;int64_t h=have0;have0=have1;have1=h;}
            else{if(!scaleConvertRow(kind,static_cast<const uint8_t *>(src)+srcStride*i,r0,sw))return false;have0=i;}
        }
        if(have1!=int64_t(j)){if(!scaleConvertRow(kind,static_cast<const uint8_t *>(src)+srcStride*j,r1,sw))return false;have1=j;}
        volatile uint32_t *out=reinterpret_cast<volatile uint32_t *>(static_cast<volatile uint8_t *>(dst)+dstStride*y);
        for(uint32_t x=0;x<dw;++x) {
            const uint32_t m=xmap[x],xi=m>>8,wx=m&255,xj=xi+1<sw?xi+1:xi;
            const uint32_t a=r0[xi],b=r0[xj],c=r1[xi],d=r1[xj];
            uint32_t px=0xff000000u;
            for(unsigned s=0;s<24;s+=8) {
                const uint32_t top=((a>>s)&255)*(256-wx)+((b>>s)&255)*wx,bot=((c>>s)&255)*(256-wx)+((d>>s)&255)*wx;
                px|=(((top*(256-wy)+bot*wy)>>16)&255)<<s;
            }
            out[x]=px;
        }
    }
    return true;
}
'''
rep("class NativeLegacyDisplay;\nstatic NativeLegacyDisplay *gDirectDisplay=nullptr;\n",
    helpers + "class NativeLegacyDisplay;\nstatic NativeLegacyDisplay *gDirectDisplay=nullptr;\n")
# snapshot: direct mode only, never with scaling
rep("        if(!directOn || directBoot || shownSlot<0 || !bootMapping || !shownStride)return;\n",
    "        if(gScaled || !directOn || directBoot || shownSlot<0 || !bootMapping || !shownStride)return;\n")
# blank swap
rep("               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||\n"
    "               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {\n",
    "               !scanoutMatches(bw,bh,blank) || blank.v_depth!=32 || blank.v_rotate ||\n"
    "               !native_legacy_surface_bounds(blank.v_width,blank.v_height,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {\n")
rep("                bootMapping->getLength(),blank.v_rowBytes,bw,bh,0xff000000U)) {\n",
    "                bootMapping->getLength(),blank.v_rowBytes,blank.v_width,blank.v_height,0xff000000U)) {\n")
# surface swap checks
rep("           video.v_width!=width || video.v_height!=height || video.v_depth!=32 || video.v_rotate ||\n"
    "           !native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {\n",
    "           !scanoutMatches(width,height,video) || video.v_depth!=32 || video.v_rotate ||\n"
    "           !native_legacy_surface_bounds(video.v_width,video.v_height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {\n")
rep("        if(directOn && !half && !xr && !gpuBypass && !latchRequest) {\n",
    "        if(directOn && !half && !xr && !gpuBypass && !latchRequest && !gScaled) {\n")
rep("        if(!gpuBypass && !xr && native_legacy_gpu_offer(this,memory,sourceStride,width,height,half?halfOfferFormat():NGP_BGRX8888)) {\n",
    "        if(!gpuBypass && !xr && !gScaled && native_legacy_gpu_offer(this,memory,sourceStride,width,height,half?halfOfferFormat():NGP_BGRX8888)) {\n")
rep("        if(half || xr) {\n            if(!native_legacy_prepare_scanout()) {OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}\n"
    "            uint64_t copyStart;__asm__ volatile(\"isb\\n\\tmrs %0, cntpct_el0\":\"=r\"(copyStart));\n"
    "            if(!(xr?convertXrRows:convertHalfRows)(",
    "        if(half || xr || gScaled) {\n            if(!native_legacy_prepare_scanout()) {OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}\n"
    "            uint64_t copyStart;__asm__ volatile(\"isb\\n\\tmrs %0, cntpct_el0\":\"=r\"(copyStart));\n"
    "            if(gScaled?!scaleRows(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),bootMapping->getLength(),video.v_rowBytes,\n"
    "                video.v_width,video.v_height,reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,\n"
    "                xr?kScaleXr:half?kScaleHalf:kScaleBGRA) :\n"
    "               !(xr?convertXrRows:convertHalfRows)(")
# startHardware: install the logical mode
rep("            installBootDimensions((uint32_t)video.v_width,(uint32_t)video.v_height);\n"
    "            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);\n"
    "            valid=width==video.v_width && height==video.v_height;\n",
    "            logicalConfigure((uint32_t)video.v_width,(uint32_t)video.v_height);\n"
    "            installBootDimensions(gLogicalW,gLogicalH);\n"
    "            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);\n"
    "            valid=width==gLogicalW && height==gLogicalH;\n"
    "            if(gScaled)printf(\"NativeLegacyDisplay: logical %ux%u scaled to the %ux%u panel\\n\",gLogicalW,gLogicalH,gPhysW,gPhysH);\n")
rep("                valid=native_legacy_gop_mapping_observed(this,bootMapping,width,height);\n",
    "                valid=native_legacy_gop_mapping_observed(this,bootMapping,(uint32_t)video.v_width,(uint32_t)video.v_height);\n")
f.write_text(t)
print('driver ok')
pk = Path('/home/xiaomi/sadnut-fuxi-local-20261103/pack-ios17-hidpath.py')
s = pk.read_text()
o = "           'offline-legacy-unit-scale=3 sheng-vsync=60 ' if ROOT_MODE else '')\n"
assert s.count(o) == 1
s = s.replace(o, "           'offline-legacy-unit-scale=3 sheng-vsync=60 '\n"
                 "           # Logical 1179x2556 (iPhone 14 Pro 393x852 pt at 3x; Xiaomi 13's 440 dpi horizontally), scaled to the 1080x2400 panel.\n"
                 "           'offline-legacy-logical-width=1179 offline-legacy-logical-height=2556 ' if ROOT_MODE else '')\n")
s = s.replace("GPU_ADDON = os.environ.get('GPU_ADDON', '/mnt/f/UniXNU-FUXI-work/ios17-cache/gpuaddon-ios17e-wallpaper7565.bin')",
              "GPU_ADDON = os.environ.get('GPU_ADDON', '/mnt/f/UniXNU-FUXI-work/ios17-cache/gpuaddon-ios17e-wallpaper7565-chained.bin')")
pk.write_text(s)
print('pack ok', 'chained' in s)
