/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "bpf-foreign.h"
#include "bpf-program.h"
#include "cgroup.h"
#include "memory-util.h"
#include "mountpoint-util.h"
#include "set.h"

typedef struct BPFForeignKey BPFForeignKey;
struct BPFForeignKey {
        uint32_t prog_id;
        uint32_t attach_type;
};

static int bpf_foreign_key_new(uint32_t prog_id,
                enum bpf_attach_type attach_type,
                BPFForeignKey **ret) {
        _cleanup_free_ BPFForeignKey *p = NULL;

        assert(ret);

        p = new(BPFForeignKey, 1);
        if (!p)
                return log_oom();

        *p = (BPFForeignKey) {
                .prog_id = prog_id,
                .attach_type = attach_type,
        };

        *ret = TAKE_PTR(p);

        return 0;
}

static int bpf_foreign_key_compare_func(const BPFForeignKey *a, const BPFForeignKey *b) {
        int r = CMP(a->prog_id, b->prog_id);
        if (r != 0)
                return r;

        return CMP(a->attach_type, b->attach_type);
}

static void bpf_foreign_key_hash_func(const BPFForeignKey *p, struct siphash *h) {
        siphash24_compress(&p->prog_id, sizeof(p->prog_id), h);
        siphash24_compress(&p->attach_type, sizeof(p->attach_type), h);
}

DEFINE_PRIVATE_HASH_OPS_FULL(bpf_foreign_by_key_hash_ops,
                BPFForeignKey, bpf_foreign_key_hash_func, bpf_foreign_key_compare_func, free,
                BPFProgram, bpf_program_unref);

static int attach_programs(Unit *u, const char *path, Hashmap* foreign_by_key, uint32_t attach_flags) {
        const BPFForeignKey *key;
        BPFProgram *prog;
        int r;

        assert(u);

        HASHMAP_FOREACH_KEY(prog, key, foreign_by_key) {
                r = bpf_program_cgroup_attach(prog, key->attach_type, path, attach_flags);
                if (r < 0)
                        return log_unit_error_errno(u, r, "Attaching foreign BPF program to cgroup %s failed: %m", path);
        }

        return 0;
}

/*
 * Prepare foreign BPF program for installation:
 * - Load the program from BPF filesystem to the kernel;
 * - Store program FD identified by program ID and attach type in the unit.
 */
static int bpf_foreign_prepare(
                Unit *u,
                enum bpf_attach_type attach_type,
                const char *bpffs_path) {
        _cleanup_(bpf_program_unrefp) BPFProgram *prog = NULL;
        _cleanup_free_ BPFForeignKey *key = NULL;
        uint32_t prog_id;
        int r;

        assert(u);
        assert(bpffs_path);

        r = bpf_program_new_from_bpffs_path(bpffs_path, &prog);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to create foreign BPFProgram: %m");

        r = bpf_program_get_id_by_fd(prog->kernel_fd, &prog_id);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to get BPF program id by fd: %m");

        r = bpf_foreign_key_new(prog_id, attach_type, &key);
        if (r < 0)
                return log_unit_error_errno(u, r,
                                "Failed to create foreign BPF program key from path '%s': %m", bpffs_path);

        r = hashmap_ensure_put(&u->bpf_foreign_by_key, &bpf_foreign_by_key_hash_ops, key, prog);
        if (r == -EEXIST) {
                log_unit_warning_errno(u, r, "Foreign BPF program already exists, ignoring: %m");
                return 0;
        }
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to put foreign BPFProgram into map: %m");

        TAKE_PTR(key);
        TAKE_PTR(prog);

        return 0;
}

int bpf_foreign_supported(void) {
        int r;

        r = cg_all_unified();
        if (r <= 0)
                return r;

        return path_is_mount_point("/sys/fs/bpf", NULL, 0);
}

int bpf_foreign_install(Unit *u) {
        _cleanup_free_ char *cgroup_path = NULL;
        CGroupBPFForeignProgram *p;
        CGroupContext *cc;
        int r;

        assert(u);

        cc = unit_get_cgroup_context(u);
        if (!cc)
                return 0;

        r = cg_get_path(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, NULL, &cgroup_path);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to get cgroup path: %m");

        LIST_FOREACH(programs, p, cc->bpf_foreign_programs) {
                r = bpf_foreign_prepare(u, p->attach_type, p->bpffs_path);
                if (r < 0)
                        return log_unit_error_errno(u, r, "Failed to prepare foreign BPF hashmap: %m");
        }

        r = attach_programs(u, cgroup_path, u->bpf_foreign_by_key, BPF_F_ALLOW_MULTI);
        if (r < 0)
                  return log_unit_error_errno(u, r, "Failed to install foreign BPF programs: %m");

        return 0;
}
