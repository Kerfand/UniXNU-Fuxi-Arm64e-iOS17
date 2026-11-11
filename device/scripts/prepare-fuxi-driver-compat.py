#!/usr/bin/env python3
"""Apply the pinned shared GPU driver's Darwin23 source compatibility patch."""
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1]
driver=root/'drivers/Adreno740'
revision=subprocess.check_output(['git','-C',driver,'rev-parse','HEAD'],text=True).strip()
if revision!='a9a2cbb43b89d672e2921e19721a6438b738b17f':
    raise SystemExit('Unexpected Adreno740 revision; requalify the compatibility patch')
patch=root/'patches/drivers/adreno740-darwin23-task-role.patch'
def check(*args):
    return subprocess.run(['git','-C',driver,'apply',*args,str(patch)],capture_output=True).returncode==0
if check('--reverse','--check'):
    print('Darwin23 GPU compatibility already applied')
elif check('--check'):
    subprocess.run(['git','-C',driver,'apply',str(patch)],check=True)
else:
    raise SystemExit('GPU source context differs; refusing partial patch application')
