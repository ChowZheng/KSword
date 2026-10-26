#!/usr/bin/env python3
"""离线调用生产C通用热键规则；不生成逐版本内核授权表。"""
from __future__ import annotations
import argparse, hashlib, json, sys, uuid
from pathlib import Path
import pefile
from win32k_rule_host import build_probe, match_pair as production_match

def identity(path: Path) -> dict:
    """身份只记录样本；版本资源或RSDS缺失不改变结构匹配。"""
    data=path.read_bytes()
    result={'file':path.name,'sha256':hashlib.sha256(data).hexdigest().upper(),'size':len(data)}
    try:
        pe=pefile.PE(data=data)
        result.update(machine=pe.FILE_HEADER.Machine,timestamp=pe.FILE_HEADER.TimeDateStamp,sizeOfImage=pe.OPTIONAL_HEADER.SizeOfImage)
        rsds=[]
        for entry in getattr(pe,'DIRECTORY_ENTRY_DEBUG',[]):
            payload=pe.get_data(entry.struct.AddressOfRawData,entry.struct.SizeOfData)
            if entry.struct.Type==2 and len(payload)>=24 and payload[:4]==b'RSDS':
                rsds.append({'guid':str(uuid.UUID(bytes_le=payload[4:20])),'age':int.from_bytes(payload[20:24],'little')})
        result['rsds']=rsds
    except pefile.PEFormatError: result['metadataError']='invalid_pe'
    return result

def match_pair(full_path: Path,base_path: Path,binary: Path|None=None) -> dict:
    """开发工具与运行时共用C实现，不复制第二套Python规则。"""
    output={'full':identity(full_path),'base':identity(base_path)}
    output['rules']=production_match(full_path,base_path,binary)
    output['status']='matched' if output['rules'].get('Matched') else 'unsupported'
    return output

def main(argv: list[str]|None=None) -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pair',nargs=2,action='append',required=True,metavar=('FULL','BASE'))
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args(argv); binary=build_probe()
    pairs=[match_pair(Path(full),Path(base),binary) for full,base in args.pair]
    matched=sum(pair['status']=='matched' for pair in pairs)
    report={'schemaVersion':2,'method':'same_production_c_semantic_rules','matchedPairs':matched,'unmatchedPairs':len(pairs)-matched,'pairs':pairs}
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'RULE_MATCHED={matched} RULE_UNMATCHED={len(pairs)-matched}')
    return 0 if matched==len(pairs) else 2

if __name__=='__main__': sys.exit(main())
