/*
 * Copyright (C) 2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _UNITS_H
#define _UNITS_H

#include "libdevmapper.h"
#include <CUnit/CUnit.h>

#define DECL(n) \
	extern CU_TestInfo n ## _list[];\
	int n ## _init(void); \
	int n ## _fini(void);

DECL(bitset);
DECL(config);
DECL(dmlist);
DECL(dmstatus);
DECL(regex);
DECL(string);

#endif
