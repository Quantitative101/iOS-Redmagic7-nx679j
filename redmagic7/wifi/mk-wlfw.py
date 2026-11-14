import re
import sys

src, release = sys.argv[1], sys.argv[2]
c = open(src + '/qmi.c').read()
h = open(src + '/qmi.h').read()
print('/* ath11k\'s qmi.h and the message tables of its qmi.c (Linux %s, BSD-3-Clause-Clear), put together by' % release)
print(' * tools/mk-wlfw.sh: not to be edited, not to be kept. */')
# the names qmi.h uses of a Linux kernel around it, outside its message definitions: nothing here does anything
print('''#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint64_t dma_addr_t;
#define __iomem
struct list_head { struct list_head *next, *prev; };
struct work_struct { int unused; };
struct workqueue_struct;
typedef struct { int unused; } spinlock_t;
typedef struct { int unused; } wait_queue_head_t;
''')
print(re.sub(r'^#include <linux/[^>]*>\n', '', h, flags=re.M))
# every table, in the file's order (a table is used by the ones after it)
print('\n'.join(re.findall(r'static const struct qmi_elem_info \w+\[\] = \{.*?\n\};\n', c, re.S)))

# What the configuration message is made of for this chip: the two tables its entry of the
# driver's parameter list (core.c) names, from hw.c, with the definitions they are written in
# (ce.h, htc.h), and from that entry how many of each table's rows are sent.
core = open(src + '/core.c').read()
hw = open(src + '/hw.c').read()
ce = open(src + '/ce.h').read()
htc = open(src + '/htc.h').read()
entry = core[core.index('"wcn6750 hw1.0"'):]
entry = entry[:entry.index('\n\t},')]
def of(field):
    return re.search(r'\.%s = (\w+),' % field, entry).group(1)
print('/* as Linux has it on a little-endian machine, whatever this kernel\'s headers define */')
print('#undef __BIG_ENDIAN\n#define __cpu_to_le32(x) (x)')
print(re.search(r'#define CE_ATTR_BYTE_SWAP_DATA.*?#endif\n', ce, re.S).group(0))
print(''.join(re.findall(r'^#define PIPEDIR_\w+.*\n', ce, re.M)))
for name in ('service_to_pipe', 'ce_pipe_config'):
    print(re.search(r'struct %s \{.*?\n\};\n' % name, ce, re.S).group(0))
print(re.search(r'enum ath11k_htc_svc_gid \{.*?\n\};\n', htc, re.S).group(0))
print(re.search(r'#define SVC\(group, idx\).*?\n.*?\n', htc).group(0))
print(re.search(r'enum ath11k_htc_svc_id \{.*?\n\};\n', htc, re.S).group(0))
print('#undef SVC')
for kind, field in (('ce_pipe_config', 'target_ce_config'), ('service_to_pipe', 'svc_to_ce_map')):
    print('static ' + re.search(r'const struct %s %s\[\] = \{.*?\n\};\n' % (kind, of(field)), hw, re.S).group(0))
print('#define WLFW_PIPES\t\t%s\n#define WLFW_PIPE_COUNT\t\t%s' % (of('target_ce_config'), of('target_ce_count')))
print('#define WLFW_SERVICES\t\t%s\n#define WLFW_SERVICE_COUNT\t%s' % (of('svc_to_ce_map'), of('svc_to_ce_map_len')))
print('#define WLFW_SENDS_INI\t\t%d\t/* fw_wmi_diag_event */' % (of('fw_wmi_diag_event') == 'true'))
print('#define WLFW_SHADOW_REGISTERS\t%d\t/* supports_shadow_regs */' % (of('supports_shadow_regs') == 'true'))
