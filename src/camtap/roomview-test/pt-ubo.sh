# pt-ubo.sh <hits>: dump the roomview UBO (x27+192, 132 floats) and the 3D flags at vrcompositor 0x161cfc
P=$(pgrep -x vrcompositor); B=$(head -1 /proc/$P/maps | cut -d- -f1)
cat > /tmp/ptg-ubo3.gdb <<G
set pagination off
handle SIGPIPE nostop noprint
handle SIG32 nostop noprint
handle SIG33 nostop noprint
set \$n = 0
break *(0x$B+0x161cfc)
commands
silent
printf "HIT %d 835=%d 836=%d 864=%d 772=%d cam=%d\n", \$n, *(unsigned char*)(\$x27+835), *(unsigned char*)(\$x27+836), *(unsigned char*)(\$x27+864), *(int*)(\$x27+772), \$w28
x/132fw \$x27+192
set \$n = \$n + 1
if \$n >= ${1:-2}
detach
quit
end
continue
end
continue
G
timeout -s INT 15 gdb -q -batch -p $P -x /tmp/ptg-ubo3.gdb 2>&1 | grep -E "^HIT|^0x"
