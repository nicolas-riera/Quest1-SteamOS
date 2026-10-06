R=/mnt/sf/p3
cat $R/usr/lib/systemd/user/pidbridge.service; echo ==; grep -n -i -B2 -A8 lepton $R/etc/bashrc.d/vrshortcuts.sh | head -60
echo "== binder in kernel"; K=$R/usr/lib/modules/6.18.0-gfbdbca41fd45
grep -iE "binder|ashmem" $K/modules.builtin $K/modules.dep | head; strings -n 8 $K/modules.builtin.modinfo | grep -iE "^binder" | head -5
echo "== steam client lepton refs"; grep -rIl -i lepton $R/usr/lib/steam $R/usr/share/steam 2>/dev/null | head; ls $R/usr/lib/steam 2>/dev/null | head
