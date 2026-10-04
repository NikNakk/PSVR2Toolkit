#!/usr/bin/env python3
"""Demultiplex observable ACL handles and extract candidate button edge markers."""
import argparse,json
from collections import Counter,defaultdict
from pathlib import Path
from analyze_sony_sense_etw_pcap import hid_reports,reports,setting_runs
from analyze_sony_optical_capture import readcsv,writecsv,Index


def main():
 ap=argparse.ArgumentParser();ap.add_argument('bluetooth',type=Path);ap.add_argument('--inventory',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);args=ap.parse_args();out=args.out
 clock=json.loads(args.inventory.read_text());qpc=lambda t:clock['qpc_origin_us']+(t-clock['unix_origin_us']-clock['clock_offset_us'])/clock['clock_slope'];origin=clock['qpc_origin_us']
 inputs=[];counts=Counter();byhandle=defaultdict(list);audit=Counter()
 for ts,typ,handle,cid,r in hid_reports(args.bluetooth,audit):
  counts[(hex(typ),str(handle),str(cid))]+=1
  if typ==0xa1:
   inputs.append(dict(host_us=qpc(ts),relative_s=(qpc(ts)-origin)/1e6,unix_us=ts,acl_handle=handle,l2cap_cid=cid,report_sequence=r[1],byte9=r[9],byte10=r[10],byte11=r[11]))
 writecsv(out/'a131-button-bytes.csv',inputs)
 commands=list(reports(args.bluetooth))
 for r in commands:r.update(host_us=qpc(r['capture_ts_us']),relative_s=(qpc(r['capture_ts_us'])-origin)/1e6);byhandle[r['acl_handle']].append(r)
 writecsv(out/'a231-all-handles.csv',commands)
 indices={h:Index(v) for h,v in byhandle.items()};frames=readcsv(out/'capture-optical-frames.csv');fi={side:Index([r for r in frames if r['side']==side]) for side in ['L','R']}
 marks=[]
 for h in sorted(set(r['acl_handle'] for r in inputs if r['acl_handle'] is not None)):
  rows=[r for r in inputs if r['acl_handle']==h]
  for field in ['byte9','byte10']:
   for bit in range(8):
    on=None;previous=0
    for r in rows:
     value=(r[field]>>bit)&1
     if value and not previous:on=r
     elif not value and previous and on:
      cmd=indices[h].previous(on['host_us']);m=dict(acl_handle=h,field=field,bit=bit,press_qpc_us=on['host_us'],press_relative_s=on['relative_s'],release_relative_s=r['relative_s'],duration_s=(r['host_us']-on['host_us'])/1e6,phase=cmd.get('phase',''),led0=cmd.get('led0',''))
      for side in ['L','R']:
       f,dt=fi[side].nearest(on['host_us'],30000);m[side+'_union']=f.get('union_count','');m[side+'_optical_dt_us']=dt
      marks.append(m);on=None
     previous=value
 writecsv(out/'button-markers.csv',sorted(marks,key=lambda r:r['press_qpc_us']))
 summary=dict(streams={str(k):v for k,v in counts.items()},container=dict(audit),button_intervals=len(marks),marker_note='Field/bit identities are candidates derived from user-reported button presses; ACL handles are validated by adjacent framing. No automatic left/right attribution.')
 (out/'bluetooth-inventory.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
