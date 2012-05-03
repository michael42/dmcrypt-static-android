# Copyright (C) 2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

flatten() {
    cat > flatten.config

    for s in `egrep '^[a-z]+ {$' flatten.config | sed -e s,{$,,`; do
	sed -e "/^$s/,/^}/p;d" flatten.config | sed -e '1d;$d' | sed -e "s,^[ \t]*,$s/,";
    done
}

lvm dumpconfig -vvvv | flatten | sort > config.dump
flatten < etc/lvm.conf | sort > config.input

# check that dumpconfig output corresponds to the lvm.conf input
diff -wu config.input config.dump

# and that merging multiple config files (through tags) works
lvm dumpconfig | flatten | not grep 'log/verbose=1'
lvm dumpconfig | flatten | grep 'log/indent=1'

aux lvmconf 'tags/@foo {}'
echo 'log { verbose = 1 }' > etc/lvm_foo.conf
lvm dumpconfig | flatten | grep 'log/verbose=1'
lvm dumpconfig | flatten | grep 'log/indent=1'

