# List framework compatibility-matrix kernel config requirements touched by kernel/steamos.config
cd ~/q1/sys/system/etc/vintf || exit 1
ls
for f in compatibility_matrix*.xml; do echo "$f: $(grep -o '<kernel version="[^"]*"' "$f" | sort -u | tr '\n' ' ')"; done
tr -d '\r' < /mnt/d/Documents/Projets/Quest1-SteamOS/kernel/steamos.config | grep -oE '^CONFIG_[A-Z0-9_]+' | while read -r o; do
  r=$(grep -h -A1 "<key>$o</key>" compatibility_matrix*.xml < /dev/null | grep -o '<value[^<]*</value>' | sort -u | tr '\n' ' ')
  [ -n "$r" ] && echo "REQ $o: $r"
done
echo done
