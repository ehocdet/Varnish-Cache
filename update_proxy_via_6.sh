#!/bin/bash

cp -p $0 /tmp/save.$$

set -eux
git reset --hard master
if ! git merge 'VRT_DirectorResolve' 'vtp_preamble'; then
	echo SHELL to resolve conflict
	bash
fi
commits=(
    # add an ipv6 bogo ip by the name b...
    c16a1b6239d9d47a10d2c9ef83696e13da9b2743
    # Basic "via" backends support
    8e57be280591d55b4933506f44b68b7d89f45e7a
    # via backends in VCL
    d5fbb7d5c3b6fcf7c127ac45c78cdd789e5aa4a1
    # Add the .authority field to backend...
    66d6712efe60a1aca302d2490d329441c7248f0e
)
for c in "${commits[@]}" ; do
    if ! git cherry-pick "${c}" ; then
	echo SHELL to resolve conflict
	bash
    fi
done

cp -p /tmp/save.$$ $0
git add $0
git commit -m 'add the update script'
