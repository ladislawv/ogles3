/*
 * pistat: the Raspberry Pi's health as its firmware reports it, for the
 * A1200 test station (the machine froze with no program running after
 * long GPU runs, docs/GLES3_PLAN.md §7): the SoC temperature and its
 * limit, the throttling and under-voltage flags (now and since the Pi
 * started), the ARM, core and V3D clocks and the core voltage. Read-only:
 * mailbox property requests only, as v3dcore's v3d_init finds the mailbox
 * (devicetree.resource); the GPU's registers are not touched.
 *   usage: pistat
 */
#include <stdio.h>
#include <exec/types.h>
#include <proto/exec.h>
#include "v3d_types.h"
#include "devicetree_protos.h"

static APTR DeviceTreeBase;
static ULONG MailBox;

#define MBOX_READ   LE32(*((const volatile ULONG *)(MailBox + 0x00)))
#define MBOX_STATUS LE32(*((const volatile ULONG *)(MailBox + 0x18)))
#define MBOX_WRITE  *((volatile ULONG *)(MailBox + 0x20))
#define MBOX_CHANNEL  8
#define MBOX_TX_FULL  (1UL << 31)
#define MBOX_RX_EMPTY (1UL << 30)
#define MBOX_CHANMASK 0xF

static ULONG storage[16 + 4];

static CONST_APTR prop_recursive(APTR key, const char *name)
{
    do {
        APTR prop = DT_FindProperty(key, name);
        if (prop)
            return DT_GetPropValue(prop);
        key = DT_GetParent(key);
    } while (key);
    return NULL;
}

/* The mailbox's address as the 68k sees it (v3d_init's steps). */
static int find_mailbox(void)
{
    APTR key = DT_OpenKey("/aliases");
    const char *alias;
    const ULONG *adr, *reg, *addr, *cpu_addr, *ranges;
    ULONG cells = 1, cpu_cells = 1;
    if (!key)
        return 0;
    alias = DT_GetPropValue(DT_FindProperty(key, "mailbox"));
    DT_CloseKey(key);
    if (!alias || !(key = DT_OpenKey(alias)))
        return 0;
    adr = prop_recursive(key, "#address-cells");
    reg = DT_GetPropValue(DT_FindProperty(key, "reg"));
    if (adr)
        cells = *adr;
    MailBox = reg[cells - 1];
    DT_CloseKey(key);
    if (!(key = DT_OpenKey("/soc")))
        return 0;
    cells = 1;
    addr = prop_recursive(key, "#address-cells");
    cpu_addr = DT_GetPropValue(DT_FindProperty(DT_OpenKey("/"), "#address-cells"));
    if (addr)
        cells = *addr;
    if (cpu_addr)
        cpu_cells = *cpu_addr;
    ranges = DT_GetPropValue(DT_FindProperty(key, "ranges"));
    MailBox = MailBox - ranges[cells - 1] + ranges[cells + cpu_cells - 1];
    DT_CloseKey(key);
    return 1;
}

/* One property tag with two value words (an id, a value): the reply's
 * value word; returns 0 when the firmware refused it. */
static int property(ULONG tag, ULONG id, ULONG *value)
{
    ULONG *p = (ULONG *)(((ULONG)storage + 15) & ~15UL), len = 8 * 4, rsp;
    p[0] = LE32(8 * 4);
    p[1] = 0;
    p[2] = LE32(tag);
    p[3] = LE32(8);
    p[4] = 0;
    p[5] = LE32(id);
    p[6] = 0;
    p[7] = 0; /* end tag */
    CachePreDMA(p, &len, 0);
    Forbid();
    while (MBOX_STATUS & MBOX_TX_FULL)
        ;
    MBOX_WRITE = LE32(((ULONG)p & ~(ULONG)MBOX_CHANMASK) | MBOX_CHANNEL);
    do {
        while (MBOX_STATUS & MBOX_RX_EMPTY)
            ;
        rsp = MBOX_READ;
    } while ((rsp & MBOX_CHANMASK) != MBOX_CHANNEL);
    Permit();
    CachePostDMA(p, &len, 0);
    if (LE32(p[1]) != 0x80000000UL)
        return 0;
    *value = LE32(p[6]);
    return 1;
}

int main(void)
{
    static const struct { const char *name; ULONG id; } clocks[] = { { "ARM", 3 }, { "core", 4 }, { "V3D", 5 } };
    ULONG v, t;
    int k;
    if (!(DeviceTreeBase = OpenResource((CONST_STRPTR) "devicetree.resource")) || !find_mailbox()) {
        printf("pistat: no devicetree.resource or mailbox (not Emu68 on a Pi?)\n");
        return 10;
    }
    if (property(0x00030006, 0, &t) && property(0x0003000a, 0, &v))
        printf("temperature %lu.%lu C (limit %lu C)\n", (unsigned long)(t / 1000), (unsigned long)(t % 1000 / 100),
               (unsigned long)(v / 1000));
    else
        printf("temperature: not reported\n");
    if (property(0x00030046, 0, &v)) /* get_throttled: bits 0-3 now, 16-19 since boot */
        printf("throttled 0x%05lx:%s%s%s%s%s%s%s%s%s\n", (unsigned long)v, (v & 1) ? " UNDER-VOLTAGE now" : "",
               (v & 2) ? " ARM-frequency-capped now" : "", (v & 4) ? " THROTTLED now" : "",
               (v & 8) ? " soft-temperature-limit now" : "", (v & 0x10000) ? " under-voltage-has-occurred" : "",
               (v & 0x20000) ? " capping-has-occurred" : "", (v & 0x40000) ? " throttling-has-occurred" : "",
               (v & 0x80000) ? " soft-temperature-limit-has-occurred" : "", v ? "" : " none");
    else
        printf("throttled: not reported\n");
    for (k = 0; k < 3; k++) {
        ULONG max = 0;
        if (property(0x00030002, clocks[k].id, &v)) {
            property(0x00030004, clocks[k].id, &max);
            printf("%s clock %lu MHz (max %lu MHz)\n", clocks[k].name, (unsigned long)(v / 1000000),
                   (unsigned long)(max / 1000000));
        }
    }
    if (property(0x00030003, 1, &v)) /* core voltage, microvolts */
        printf("core voltage %lu.%04lu V\n", (unsigned long)(v / 1000000), (unsigned long)(v % 1000000 / 100));
    return 0;
}
