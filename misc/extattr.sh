#!/bin/sh

#
# Copyright (c) 2009 Peter Holm <pho@FreeBSD.org>
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#
# $FreeBSD$
#

# Regression test of extattr on a UFS2 FS using ACLs
# Caused a "Duplicate free" panic.

[ `id -u ` -ne 0 ] && echo "Must be root!" && exit 1

. ../default.cfg

odir=`pwd`

cd /tmp
sed '1,/^EOF/d' < $odir/$0 > extattr.c
cc -o extattr -Wall extattr.c
rm -f extattr.c
cd $odir

mount | grep "${mntpoint}" | grep -q md${mdstart}${part} && umount $mntpoint
mdconfig -l | grep -q md$mdstart &&  mdconfig -d -u $mdstart

mdconfig -a -t swap -s 20m -u $mdstart
disklabel -r -w md$mdstart auto

newfs -O 2 md${mdstart}${part} > /dev/null
mount /dev/md${mdstart}${part} $mntpoint

mkdir -p ${mntpoint}/.attribute/system
cd ${mntpoint}/.attribute/system

extattrctl initattr -p . 388 posix1e.acl_access
extattrctl initattr -p . 388 posix1e.acl_default
cd /
umount /mnt
tunefs -a enable /dev/md${mdstart}${part}
mount /dev/md${mdstart}${part} $mntpoint
mount | grep md${mdstart}${part}

touch $mntpoint/acl-test
setfacl -b $mntpoint/acl-test
setfacl -m user:nobody:rw-,group:wheel:rw- $mntpoint/acl-test

for i in `jot 5`; do
	/tmp/extattr $mntpoint/acl-test &
done
for i in `jot 5`; do
	wait
done

umount $mntpoint
mdconfig -d -u $mdstart
rm -f /tmp/extattr
exit
EOF
#include <sys/types.h>
#include <sys/stat.h>
#include <err.h>

int
main(int argc, char **argv)
{
	int i;
	struct stat sb;

	for (i = 0; i < 100000; i++)
		if (lstat(argv[1], &sb) == -1)
			err(1, "lstat(%s)", argv[1]);
	return (0);
}
