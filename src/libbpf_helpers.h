/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBBPF_HELPERS_H
#define __LIBBPF_HELPERS_H

#include <bpf/libbpf.h>

int load_obj_file(struct bpf_prog_load_attr *attr,
                  struct bpf_object **obj,
                  const char *objfile, bool user_set);

int attach_to_dev(int idx, int prog_fd, const char *dev);
int detach_from_dev(int idx, const char *dev);

int attach_to_dev_tx(int idx, int prog_fd, const char *dev);
int detach_from_dev_tx(int idx, const char *dev);

#endif