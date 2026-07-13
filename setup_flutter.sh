#!/bin/bash
set -e
rm -rf /tmp/flutter_copy
cp -a /Users/aleksandr/flutter /tmp/flutter_copy
echo "copy ok"
/tmp/flutter_copy/bin/flutter --version 2>&1 | head -5
