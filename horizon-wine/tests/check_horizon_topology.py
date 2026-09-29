from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "dlls/ntdll/unix/system.c").read_text()
helpers = source[source.index("static BOOL grow_logical_proc_buf(void)"):
                 source.index("#ifdef linux", source.index("static BOOL grow_logical_proc_buf(void)"))]
switch = source.index("#elif defined(__SWITCH__)\n\nstatic NTSTATUS create_logical_proc_info(void)")
builder = source[switch:source.index("\n#else", switch)].split("\n", 1)[1]
legacy = source[source.index("    case SystemLogicalProcessorInformation:  /* 73 */"):
                source.index("    case SystemFirmwareTableInformation:  /* 76 */")]
ex_start = source.index("    case SystemLogicalProcessorInformationEx:")
extended = source[ex_start:source.index("    case SystemCpuSetInformation:", ex_start)]

fixture = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
static ULONG *performance_cores;
static unsigned int performance_cores_capacity;
static SYSTEM_LOGICAL_PROCESSOR_INFORMATION *logical_proc_info;
static unsigned int logical_proc_info_len, logical_proc_info_alloc_len;
static SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *logical_proc_info_ex;
static unsigned int logical_proc_info_ex_size, logical_proc_info_ex_alloc_size;
static ULONG_PTR system_cpu_mask, exposed_mask;
static int fail_at = -1, allocations;
static ULONG_PTR horizon_get_system_affinity_mask(void) { return exposed_mask; }
static void *test_realloc(void *p, size_t size)
{
    if (allocations++ == fail_at) return NULL;
    return realloc(p, size);
}
#define realloc test_realloc
#define pthread_once(a, b) ((void)0)
'''
fixture += helpers + builder + r'''
#undef realloc
static NTSTATUS query_legacy(void *info, ULONG size, ULONG *retlen)
{
    NTSTATUS ret = STATUS_SUCCESS;
    ULONG len = 0;
    switch (SystemLogicalProcessorInformation) {
''' + legacy + r'''
    }
    *retlen = len;
    return ret;
}
static NTSTATUS query_extended(void *query, ULONG query_len, void *info, ULONG size, ULONG *retlen)
{
    NTSTATUS ret = STATUS_SUCCESS;
    ULONG len = 0;
    switch (SystemLogicalProcessorInformationEx) {
''' + extended + r'''
    }
    *retlen = len;
    return ret;
}
static void reset(ULONG_PTR mask, int failure)
{
    free(logical_proc_info); free(logical_proc_info_ex);
    logical_proc_info = NULL; logical_proc_info_ex = NULL;
    logical_proc_info_len = logical_proc_info_alloc_len = 0;
    logical_proc_info_ex_size = logical_proc_info_ex_alloc_size = 0;
    system_cpu_mask = 0; exposed_mask = mask; allocations = 0; fail_at = failure;
}
static void check(ULONG_PTR mask)
{
    unsigned cores = 0, packages = 0, numa = 0, l1i = 0, l1d = 0, l2 = 0, groups = 0;
    ULONG bytes = 0, ex_bytes, num = count_bits(mask);
    ULONG_PTR core_mask = 0;
    LOGICAL_PROCESSOR_RELATIONSHIP rel;
    reset(mask, -1);
    assert(create_logical_proc_info() == STATUS_SUCCESS);
    assert(system_cpu_mask == mask);
    assert(query_legacy(NULL, 0, &bytes) == STATUS_INFO_LENGTH_MISMATCH);
    assert(bytes == (num * 3 + 3) * sizeof(*logical_proc_info));
    void *buffer = malloc(bytes);
    assert(query_legacy(buffer, bytes - 1, &bytes) == STATUS_INFO_LENGTH_MISMATCH);
    assert(query_legacy(NULL, bytes, &bytes) == STATUS_ACCESS_VIOLATION);
    assert(query_legacy(buffer, bytes, &bytes) == STATUS_SUCCESS);
    assert(!memcmp(buffer, logical_proc_info, bytes));
    for (unsigned i = 0; i < logical_proc_info_len; ++i)
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION *p = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)buffer + i;
        assert(p->ProcessorMask && !(p->ProcessorMask & ~mask));
        switch (p->Relationship)
        {
        case RelationProcessorCore:
            assert(count_bits(p->ProcessorMask) == 1 && !p->ProcessorCore.Flags);
            assert(!(core_mask & p->ProcessorMask)); core_mask |= p->ProcessorMask; cores++; break;
        case RelationProcessorPackage: assert(p->ProcessorMask == mask); packages++; break;
        case RelationNumaNode: assert(p->ProcessorMask == mask && p->NumaNode.NodeNumber == 0); numa++; break;
        case RelationCache:
            assert(p->Cache.LineSize == 64);
            if (p->Cache.Level == 2)
            {
                assert(p->ProcessorMask == mask && p->Cache.Type == CacheUnified);
                assert(p->Cache.Size == 2 * 1024 * 1024 && p->Cache.Associativity == 16); l2++;
            }
            else
            {
                assert(p->Cache.Level == 1 && count_bits(p->ProcessorMask) == 1);
                if (p->Cache.Type == CacheInstruction)
                { assert(p->Cache.Size == 48 * 1024 && p->Cache.Associativity == 3); l1i++; }
                else
                { assert(p->Cache.Type == CacheData && p->Cache.Size == 32 * 1024 && p->Cache.Associativity == 2); l1d++; }
            }
            break;
        default: abort();
        }
    }
    assert(cores == num && core_mask == mask && packages == 1 && numa == 1);
    assert(l1i == num && l1d == num && l2 == 1);
    free(buffer);
    rel = RelationAll;
    assert(query_extended(NULL, 0, NULL, 0, &ex_bytes) == STATUS_INVALID_PARAMETER);
    assert(query_extended(&rel, sizeof(rel), NULL, 0, &ex_bytes) == STATUS_INFO_LENGTH_MISMATCH);
    assert(ex_bytes == logical_proc_info_ex_size);
    buffer = malloc(ex_bytes);
    assert(query_extended(&rel, sizeof(rel), buffer, ex_bytes, &ex_bytes) == STATUS_SUCCESS);
    assert(!memcmp(buffer, logical_proc_info_ex, ex_bytes));
    for (unsigned off = 0; off < ex_bytes; )
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *p = (void *)((char *)buffer + off);
        assert(p->Size && p->Size <= ex_bytes - off);
        if (p->Relationship == RelationGroup)
        {
            assert(p->Group.MaximumGroupCount == 1 && p->Group.ActiveGroupCount == 1);
            assert(p->Group.GroupInfo[0].ActiveProcessorCount == num);
            assert(p->Group.GroupInfo[0].ActiveProcessorMask == mask); groups++;
        }
        off += p->Size;
    }
    assert(groups == 1);
    rel = RelationProcessorCore;
    assert(query_extended(&rel, sizeof(rel), buffer, ex_bytes, &bytes) == STATUS_SUCCESS);
    assert(bytes == num * log_proc_ex_size_plus(sizeof(PROCESSOR_RELATIONSHIP)));
    free(buffer);
}
int main(void)
{
    check(1); check(7); check(5); check(15);
    int count = allocations;
    for (int i = 0; i < count; ++i)
    {
        reset(15, i);
        assert(create_logical_proc_info() == STATUS_NO_MEMORY);
    }
    reset(0, -1);
    puts("Horizon topology: masks, cores, caches, NUMA, groups, legacy/Ex query buffers and allocation failures passed");
}
'''

with tempfile.TemporaryDirectory(prefix="horizon-topology-") as directory:
    test = Path(directory) / "topology"
    c = test.with_suffix(".c")
    c.write_text(fixture)
    subprocess.run(["clang", "-std=gnu11", "-g", "-O1", "-fshort-wchar", "-fms-extensions",
                    "-fsanitize=address,undefined", "-D__WINESRC__", "-D_WIN64",
                    "-I" + str(root / "include"), str(c), "-o", str(test)], check=True)
    subprocess.run([str(test)], check=True, timeout=30)
