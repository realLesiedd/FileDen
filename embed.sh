#!/bin/sh
# regenerate the embedded headers after editing web/index.html or assets/
cd "$(dirname "$0")" || exit 1
hexc() { python3 -c 'import sys,re;f=sys.argv[1];b=open(f,"rb").read();n=re.sub(r"[^0-9A-Za-z]","_",f.split("/")[-1])
print("unsigned char %s[] = {"%n);print(",\n".join("  "+", ".join("0x%02x"%x for x in b[i:i+12]) for i in range(0,len(b),12)));print("};\nunsigned int %s_len = %d;"%(n,len(b)))' "$1"; }
hexc web/index.html > index_html.h
hexc assets/logo192.png > app/logo.h
