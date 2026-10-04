#!/usr/bin/env python3
"""Capture-side inventory with explicit side grouping; no Bluetooth assumptions."""
import argparse,struct,json
from collections import Counter,defaultdict
from pathlib import Path
import numpy as np
from analyze_sony_optical_capture import readcsv,writecsv,profile
from analyze_psvr2_led_detector_usb import iter_records
from extract_psvr2_vi_lanes import parse_header


def main():
 ap=argparse.ArgumentParser();ap.add_argument('capture',type=Path);ap.add_argument('--out',type=Path,required=True);args=ap.parse_args();out=args.out;out.mkdir(parents=True,exist_ok=True)
 data={n:readcsv(args.capture/(n+'.csv')) for n in ['events','poses','sony_led_ground_truth','camera_frames','clock_sync']}
 clocks=data['clock_sync'];x=np.array([int(r['qpc_us']) for r in clocks]);y=np.array([int(r['unix_us']) for r in clocks]);a,b=np.polyfit(x-x[0],y-y[0],1);res=y-y[0]-(a*(x-x[0])+b)
 origin=int(x[0]);summary=dict(capture=str(args.capture),qpc_origin_us=origin,unix_origin_us=int(y[0]),clock_slope=float(a),clock_offset_us=float(b),clock_rms_us=float(np.sqrt(np.mean(res**2))),clock_max_abs_us=float(abs(res).max()))
 streams=[];frames=defaultdict(list)
 for r in data['sony_led_ground_truth']:frames[(r['side'],int(r['frame_index']))].append(r)
 fs=[]
 for (side,frame),rows in sorted(frames.items()):
  mask=0
  for r in rows:mask|=int(r['matched_mask'],0)
  fs.append(dict(side=side,frame_index=frame,host_us=min(int(r['host_us']) for r in rows),relative_s=(min(int(r['host_us']) for r in rows)-origin)/1e6,camera_rows=len(rows),union_mask=hex(mask),union_count=mask.bit_count()))
 writecsv(out/'capture-optical-frames.csv',fs)
 episodes=[];windows=[]
 for side in ['L','R','HMD']:
  poses=sorted([r for r in data['poses'] if r['device']==side],key=lambda r:int(r['host_us']));streams.append(profile('poses_'+side,[int(r['host_us']) for r in poses]))
  ep=[];buckets=defaultdict(list)
  for r in poses:
   t=int(r['host_us']);key=(r['pose_valid'],r['tracking_result'],r['connected'])
   if not ep or key!=ep[-1]['key']:
    if ep:ep[-1]['end_qpc_us']=t
    ep.append(dict(side=side,key=key,start_qpc_us=t,end_qpc_us=t,samples=0))
   ep[-1]['end_qpc_us']=t;ep[-1]['samples']+=1
   buckets[int((t-origin)//2000000)].append(r)
  for e in ep:episodes.append(dict(side=side,start_relative_s=(e['start_qpc_us']-origin)/1e6,duration_s=(e['end_qpc_us']-e['start_qpc_us'])/1e6,pose_valid=e['key'][0],tracking_result=e['key'][1],connected=e['key'][2],samples=e['samples']))
  for bucket,rows in sorted(buckets.items()):
   valid=[r for r in rows if r['pose_valid']=='1'];xyz=np.array([[float(r[k]) for k in ['px','py','pz']] for r in valid])
   qs=np.array([[float(r[k]) for k in ['qw','qx','qy','qz']] for r in valid]); qs=qs/np.linalg.norm(qs,axis=1,keepdims=True) if len(qs) else qs; angular=2*np.degrees(np.arccos(np.clip(abs(qs@qs[0]),0,1))).max() if len(qs) else None
   windows.append(dict(side=side,start_relative_s=bucket*2,pose_samples=len(rows),valid_fraction=len(valid)/len(rows),position_range_m=float(np.linalg.norm(np.ptp(xyz,axis=0))) if len(xyz) else '',orientation_range_from_first_deg=float(angular) if angular is not None else ''))
  semantic=[r for r in fs if r['side']==side]
  if semantic:streams.append(profile('optical_frames_'+side,[r['host_us'] for r in semantic]))
 writecsv(out/'capture-tracking-episodes.csv',episodes);writecsv(out/'capture-motion-windows.csv',windows)
 vi_errors=[]
 for r in data['camera_frames']:
  path=args.capture/r['filename'];raw=path.read_bytes();h=parse_header(raw)
  if len(raw)!=int(r['size']) or any(h[k]!=int(r[k]) for k in ['vts_us','sequence_id','camera_set']):vi_errors.append(path.name)
 streams.append(profile('selected_VI',[int(r['host_us']) for r in data['camera_frames']]))
 times=[];counters=[];counts=Counter()
 for r in iter_records(args.capture/'usb-if8-led-detector.bin'):
  times.append(r.host_us);counters.append(struct.unpack_from('<I',r.payload,20)[0]);counts[len(r.payload)]+=1
 streams.append(profile('IF8',times));gaps=np.diff(counters)
 summary.update(ground_truth_sides=dict(Counter(r['side'] for r in data['sony_led_ground_truth'])),vi_errors=vi_errors,if8_payload_sizes=dict(counts),if8_counter_missing=int(np.maximum(gaps-1,0).sum()),if8_counter_nonunit_jumps=dict(Counter(map(int,gaps[gaps!=1]))),if8_bytes=(args.capture/'usb-if8-led-detector.bin').stat().st_size,if8_end_relative_s=(times[-1]-origin)/1e6,selected_vi_end_relative_s=(int(data['camera_frames'][-1]['host_us'])-origin)/1e6)
 writecsv(out/'capture-stream-summary.csv',streams);(out/'capture-inventory.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
