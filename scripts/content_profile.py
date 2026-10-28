"""Prepare explicit offline AppContent metadata; SKU is a probe setting, not a license check."""
import argparse
import json
import struct
from pathlib import Path
from prepare import sfo


def find_addons(folder):
    """Extracted DLC packages under folder: {entitlement label: package root}."""
    addons={}
    if folder is None or not folder.is_dir():
        return addons
    for entry in sorted(folder.iterdir()):
        for root in (entry, entry/'Image0'):
            path=root/'sce_sys/param.sfo'
            if not path.is_file():
                continue
            content_id=str(sfo(path.read_bytes()).get('CONTENT_ID',''))
            label=content_id[-16:]
            if len(content_id)!=36 or not label.isalnum():
                raise ValueError(f'unexpected DLC content ID {content_id!r} in {path}')
            addons[label]=root.resolve()
            break
    return addons


def prepare(game, out, sku='full', addons=()):
    values=sfo((game/'sce_sys/param.sfo').read_bytes())
    params=[values.get(f'USER_DEFINED_PARAM_{i}',0) for i in range(1,5)]
    if any(type(v) is not int or not 0<=v<=0xffffffff for v in params):
        raise ValueError('invalid user-defined parameter')
    profile=dict(title_id=values.get('TITLE_ID'),sku=sku,sku_source='explicit probe setting',
                 user_params=params, mounted_addons=sorted(addons),boot_attr=0)
    out.mkdir(parents=True,exist_ok=True)
    (out/'content.bin').write_bytes(struct.pack('<8s5I',b'PS4RTC01',{'full':3,'trial':1}[sku],*params))
    (out/'content-profile.json').write_text(json.dumps(profile,indent=2)+'\n')
    print(f'AppContent profile: SKU={sku} (probe setting), user params={params}, mounted add-ons={len(addons)}')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('game',type=Path)
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--sku',choices=['full','trial'],default='full')
    a=p.parse_args();prepare(a.game,a.out,a.sku)
