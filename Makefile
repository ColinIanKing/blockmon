#
# Copyright (C) 2015-2016 Colin Ian King
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# Author: Colin Ian King <colin.i.king@gmail.com>
#

VERSION=0.01.08

CFLAGS += -Wall -Wextra -DVERSION='"$(VERSION)"' -O0 -g
LDFLAGS += -lncurses -pthread

BINDIR=/usr/sbin
MANDIR=/usr/share/man/man8

SRC = blockmon.c
OBJS = $(SRC:.c=.o)

blockmon: $(OBJS) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

blockmon.o: blockmon.c Makefile

blockmon.8.gz: blockmon.8
	gzip -c $< > $@

dist:
	rm -rf blockmon-$(VERSION)
	mkdir blockmon-$(VERSION)
	cp -rp README Makefile blockmon.c blockmon.8 perf.c perf.h COPYING blockmon-$(VERSION)
	tar -zcf blockmon-$(VERSION).tar.gz blockmon-$(VERSION)
	rm -rf blockmon-$(VERSION)

clean:
	rm -f blockmon blockmon.o blockmon.8.gz blockmon-$(VERSION).tar.gz

install: blockmon blockmon.8.gz
	mkdir -p ${DESTDIR}${BINDIR}
	cp blockmon ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}
	cp blockmon.8.gz ${DESTDIR}${MANDIR}
