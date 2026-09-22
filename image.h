#ifndef GRW_IMAGE_H
#define GRW_IMAGE_H

#include <windows.h>
#include <stdint.h>

#define SH_LINK_BASE 0x140000000ULL

#define SH_LEGACY_TIMESTAMP  0x6A7C5143u
#define SH_LEGACY_IMAGE_SIZE 0x18B09000u

#define SH_TU25_TIMESTAMP    0x6A99768Au
#define SH_TU25_IMAGE_SIZE   0x185BA000u

extern uint32_t ShTu25PoolRvaShared(void);
extern uint64_t ShTu25SpecialAddress(uint64_t legacyRva);

static uint64_t ShImageBase(void) {
    static uint64_t base = 0;

    if (!base)
        base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);

    return base;
}

static const IMAGE_NT_HEADERS64 *ShImageNt(void) {
    uint64_t b = ShImageBase();
    const IMAGE_DOS_HEADER *dos;

    if (!b)
        return NULL;

    dos = (const IMAGE_DOS_HEADER *)(uintptr_t)b;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;

    return (const IMAGE_NT_HEADERS64 *)
        (uintptr_t)(b + (uint64_t)dos->e_lfanew);
}

static uint32_t ShImageTimestamp(void) {
    const IMAGE_NT_HEADERS64 *nt = ShImageNt();

    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    return nt->FileHeader.TimeDateStamp;
}

static uint64_t ShImageSize(void) {
    static uint64_t size = 0;
    const IMAGE_NT_HEADERS64 *nt;

    if (size)
        return size;

    nt = ShImageNt();

    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    size = nt->OptionalHeader.SizeOfImage;
    return size;
}

static int ShIsTU25Build(void) {
    return ShImageTimestamp() == SH_TU25_TIMESTAMP &&
           ShImageSize() == SH_TU25_IMAGE_SIZE;
}

static int ShIsLegacyBuild(void) {
    return ShImageTimestamp() == SH_LEGACY_TIMESTAMP &&
           ShImageSize() == SH_LEGACY_IMAGE_SIZE;
}

static uint64_t ShImageAddr(uint64_t rva) {
    uint64_t special;

    if (!ShIsTU25Build())
        return ShImageBase() + rva;

    special = ShTu25SpecialAddress(rva);

    if (special)
        return special;

    switch (rva) {
    case 0x04B87978ULL:
        rva = 0x04B879F8ULL;
        break;

    case 0x0483B650ULL:
        rva = 0x0483B920ULL;
        break;

    case 0x04BB6438ULL:
        rva = 0x04BB64B8ULL;
        break;

    case 0x04D84E98ULL:
        rva = 0x04D84F18ULL;
        break;

    case 0x039C6FC8ULL:
        rva = 0x039C6DF8ULL;
        break;

    case 0x07E888FEULL:
        rva = 0x081E0B7EULL;
        break;

    case 0x010D8890ULL:
        rva = 0x010D8E20ULL;
        break;

    case 0x013781B0ULL:
        rva = 0x013796D0ULL;
        break;

    case 0x0D7C0610ULL:
        rva = 0x0D67FFA0ULL;
        break;

    case 0x07E889A2ULL:
        rva = 0x081E0C22ULL;
        break;

    case 0x14D387E3ULL:
        rva = 0x14703F83ULL;
        break;

    case 0x029B4970ULL:
        rva = 0x029B4E00ULL;
        break;

    case 0x16990140ULL:
        rva = 0x163CA8C0ULL;
        break;

    case 0x169B7630ULL:
        rva = 0x163F18D0ULL;
        break;

    case 0x0FBE88D0ULL:
        rva = 0x0FBB3580ULL;
        break;

    case 0x14393508ULL:
        rva = 0x13BEAC48ULL;
        break;

    case 0x14E7625CULL:
        rva = 0x1485806CULL;
        break;

    case 0x1880FBB0ULL:
        rva = 0x182B2B90ULL;
        break;

    case 0x1880FBC0ULL:
        rva = 0x182B2BA0ULL;
        break;

    case 0x0E064390ULL:
        rva = 0x016BF890ULL;
        break;

    case 0x060ACBF0ULL:
        rva = 0x00152DF0ULL;
        break;

    case 0x04D78D00ULL:
        rva = (uint64_t)ShTu25PoolRvaShared();

        if (!rva)
            return 0;

        break;

    case 0x032EEC50ULL:
        rva = 0x032EE140ULL;
        break;

    case 0x032EECD0ULL:
        rva = 0x032EE1C0ULL;
        break;

    case 0x173F8400ULL:
        rva = 0x16B98D80ULL;
        break;

    case 0x173F8C60ULL:
        rva = 0x16B99A30ULL;
        break;

    case 0x173F9160ULL:
        rva = 0x032EE530ULL;
        break;

    case 0x173F9930ULL:
        rva = 0x032EE5A0ULL;
        break;

    case 0x173FA280ULL:
        rva = 0x16B9B440ULL;
        break;

    case 0x173FA610ULL:
        rva = 0x16B9BB20ULL;
        break;

    case 0x032EEFB0ULL:
        rva = 0x032EE4A0ULL;
        break;

    case 0x032F4D00ULL:
        rva = 0x032F4230ULL;
        break;

    case 0x0F93CA90ULL:
        rva = 0x025C9530ULL;
        break;

    case 0x036206D0ULL:
        rva = 0x017BB100ULL;
        break;

    case 0x03287730ULL:
        rva = 0x03286FF0ULL;
        break;

    case 0x03A05AA0ULL:
        rva = 0x03A05A00ULL;
        break;

    case 0x032F5E70ULL:
        rva = 0x032F5310ULL;
        break;

    case 0x17408BA0ULL:
        rva = 0x032F3510ULL;
        break;

    case 0x0336F0B0ULL:
        rva = 0x0336E440ULL;
        break;

    case 0x0336F7F0ULL:
        rva = 0x0336EB60ULL;
        break;

    case 0x0336F7B0ULL:
        rva = 0x0336EB20ULL;
        break;

    case 0x03336E30ULL:
        rva = 0x03336260ULL;
        break;

    case 0x03336EE0ULL:
        rva = 0x03336310ULL;
        break;

    case 0x0336E7F0ULL:
        rva = 0x0336DB60ULL;
        break;

    case 0x0336EC80ULL:
        rva = 0x0336E010ULL;
        break;

    case 0x0336EC30ULL:
        rva = 0x0336DFC0ULL;
        break;

    case 0x032F3DD0ULL:
        rva = 0x032F3310ULL;
        break;

    case 0x033366A0ULL:
        rva = 0x03335AD0ULL;
        break;

    case 0x03337140ULL:
        rva = 0x03336570ULL;
        break;

    case 0x03CF89D0ULL:
        rva = 0x03CF8930ULL;
        break;

    case 0x03CF09C0ULL:
        rva = 0x03CF0920ULL;
        break;

    case 0x03CF09F8ULL:
        rva = 0x03CF0958ULL;
        break;

    case 0x03CF1660ULL:
        rva = 0x03CF15C0ULL;
        break;

    case 0x03D052C8ULL:
        rva = 0x03D05228ULL;
        break;

    case 0x03D04EA0ULL:
        rva = 0x03D04E00ULL;
        break;

    case 0x04495E90ULL:
        rva = 0x04495EB0ULL;
        break;

    case 0x039D20B8ULL:
        rva = 0x039D1F78ULL;
        break;

    case 0x04B9B4F8ULL:
        rva = 0x04B9B588ULL;
        break;

    case 0x0E0E0C70ULL:
        rva = 0x0E536F10ULL;
        break;

    case 0x049E2AF0ULL:
        rva = 0x049E2B70ULL;
        break;

    case 0x049E2A50ULL:
        rva = 0x049E2AD0ULL;
        break;

    case 0x02827B50ULL:
        rva = 0x02827F10ULL;
        break;

    case 0x027CBEA0ULL:
        rva = 0x027CC0C0ULL;
        break;

    case 0x0C6BDE10ULL:
        rva = 0x0C46B7B0ULL;
        break;

    case 0x04B98DF0ULL:
        rva = 0x04B98E80ULL;
        break;

    case 0x03AA1D79ULL:
        rva = 0x03AA1D09ULL;
        break;

    case 0x04B98F10ULL:
        rva = 0x04B98FA0ULL;
        break;

    case 0x0DC4D920ULL:
        rva = 0x0DF5C480ULL;
        break;

    case 0x0DCC7280ULL:
        rva = 0x0E04F670ULL;
        break;

    case 0x0DC79B00ULL:
        rva = 0x0DF7FEB0ULL;
        break;

    case 0x014FEEB0ULL:
        rva = 0x014FFE70ULL;
        break;

    case 0x032FDDD0ULL:
        rva = 0x032FD280ULL;
        break;

    case 0x032FDF70ULL:
        rva = 0x032FD420ULL;
        break;

    case 0x04D5B058ULL:
        rva = 0x04D5B0D8ULL;
        break;

    case 0x03905CF0ULL:
        rva = 0x03905C18ULL;
        break;

    case 0x03ACBBD8ULL:
        rva = 0x03ACBB58ULL;
        break;

    case 0x03BCB3B8ULL:
        rva = 0x03BCB2A8ULL;
        break;

    case 0x03B5ADD8ULL:
        rva = 0x03B5AC88ULL;
        break;

    default:
        break;
    }

    return ShImageBase() + rva;
}

#define SH_IMG(rva) \
    ShImageAddr((uint64_t)(rva))

static int ShInImage(uint64_t addr) {
    uint64_t b = ShImageBase();
    uint64_t s = ShImageSize();

    return b && s &&
           addr >= b &&
           addr < b + s;
}

#endif