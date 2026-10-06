#!/usr/bin/env python3
"""f_ncm (4.4): ndp_sign is only set by SET_CRC_MODE, which Windows' UsbNcm never
sends, so every OUT NTB fails with "Wrong NDP SIGN" (and IN NDPs carry a zero sign).
Initialise it on reset and keep it in sync with SET_NTB_FORMAT.
usage: fix_ncm.py <kernel tree>   (idempotent)"""
import os
import sys

p = os.path.join(sys.argv[1], "drivers/usb/gadget/function/f_ncm.c")
s = open(p).read()
if "holo: ndp_sign" in s:
    sys.exit(0)
old = "\tncm->parser_opts = &ndp16_opts;\n\tncm->is_crc = false;\n"
assert old in s
s = s.replace(old, old + "\tncm->ndp_sign = ncm->parser_opts->ndp_sign;\t/* holo: ndp_sign */\n", 1)

for fmt in ("ndp16_opts", "ndp32_opts"):
    old = f"\t\t\tncm->parser_opts = &{fmt};\n"
    assert old in s, fmt
    s = s.replace(old, old + "\t\t\tncm->ndp_sign = ncm->parser_opts->ndp_sign |\n"
                  "\t\t\t\t(ncm->is_crc ? NCM_NDP_HDR_CRC : NCM_NDP_HDR_NOCRC);\n", 1)
open(p, "w").write(s)
print("patched", p)
