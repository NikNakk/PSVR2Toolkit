#!/usr/bin/env python3
"""Test observable IF8 repeated-section hypothesis against ALL packets and labels.
No private Sony layouts. Candidate boxes/section-camera mapping remain hypotheses.
"""
import argparse,csv,json,struct
from collections import Counter,defaultdict
from pathlib import Path
import numpy as np
from analyze_psvr2_led_detector_usb import iter_records
from analyze_sony_optical_capture import readcsv,writecsv,Index


def main():
 ap=argparse.ArgumentParser(); ap.add_argument('capture',type=Path); ap.add_argument('--out',type=Path,default=Path('analysis/sony-oracle-20261004')); ap.add_argument('--controller-side',choices=['L','R']); args=ap.parse_args(); out=args.out
 packetrows=readcsv(out/'if8-packets.csv'); gt=readcsv(args.capture/'sony_led_ground_truth.csv');
 if args.controller_side is None and len({r['side'] for r in gt})>1: raise ValueError('Multiple semantic controller sides: specify --controller-side')
 gt=[r for r in gt if args.controller_side is None or r['side']==args.controller_side]; counts=[]; blobs=[]; times=[]; stamps=[]; counters=[]; audit=Counter(); types=Counter(); bounds=Counter(); slots=[Counter() for _ in range(4)]; nonzero_tails=Counter()
 # 64 + 4 * (4 + 256 * 36) = 36944, inferred from repeating markers/zero tails.
 for n,r in enumerate(iter_records(args.capture/'usb-if8-led-detector.bin')):
  times.append(r.host_us); stamps.append(struct.unpack_from('<I',r.payload,8)[0]); counters.append(struct.unpack_from('<I',r.payload,20)[0]); cs=[]; bs=[]
  for slot in range(4):
   off=64+slot*9220; count=struct.unpack_from('<I',r.payload,off)[0]; cs.append(count); slots[slot][count]+=1
   if count>256: raise ValueError('section count > capacity')
   tail=r.payload[off+4+count*36:off+9220]; nonzero_tails[slot]+=bool(any(tail))
   items=[]
   for j in range(count):
    rec=r.payload[off+4+j*36:off+4+(j+1)*36]
    kind,a,b,x0,x1,y0,y1,*rest=struct.unpack('<HBBHHHHIIIIII',rec)
    types[kind]+=1; audit['records']+=1
    valid=0<=x0<=x1<=508 and 0<=y0<=y1<=508
    audit['candidate_bbox_valid']+=valid
    bounds[(slot,kind)]+=1; items.append((x0,x1,y0,y1))
   bs.append(items)
  counts.append(cs); blobs.append(bs)
  packetrows[n].update({f'section{k}_count':cs[k] for k in range(4)})
 writecsv(out/'if8-packets.csv',packetrows)
 times=np.array(times); counts=np.array(counts); stamps=np.array(stamps); counters=np.array(counters)
 differences=np.diff(counters); gaps=[]
 for i in np.where(differences!=1)[0]: gaps.append(dict(packet_index=int(i+2),host_us=int(times[i+1]),counter_before=int(counters[i]),counter_after=int(counters[i+1]),delta=int(differences[i]),host_delta_us=int(times[i+1]-times[i]),device_delta_us=int(stamps[i+1]-stamps[i])))
 writecsv(out/'if8-gaps.csv',gaps)
 # All matched semantic blob indices compared to count bounds; test lag and base explicitly.
 matches=[]; correlations=[]; associations=[]
 for cam in range(4):
  rows=[r for r in gt if int(r['camera'])==cam]; ts=np.array([int(r['host_us']) for r in rows]); idx=np.searchsorted(times,ts); idx=np.clip(idx,1,len(times)-1); idx=np.where(abs(times[idx-1]-ts)<abs(times[idx]-ts),idx-1,idx)
  matched=np.array([int(r['matched_mask'],0).bit_count() for r in rows]); near=abs(times[idx]-ts)<30000
  for slot in range(4):
   for lag in range(-3,4):
    ii=np.clip(idx+lag,0,len(times)-1); cc=counts[ii,slot]
    corr=float(np.corrcoef(cc[near],matched[near])[0,1]) if np.std(cc[near]) and np.std(matched[near]) else 0
    correlations.append(dict(camera=cam,section=slot,packet_lag=lag,rows=int(near.sum()),correlation=corr))
    for base in [0,1]:
     total=good=0
     for r,ct,ok in zip(rows,cc,near):
      if not ok: continue
      mask=int(r['matched_mask'],0)
      for led in range(17):
       if mask&(1<<led):
        v=int(r[f'blob_{led}'])-base; total+=1; good+=0<=v<ct
     matches.append(dict(camera=cam,section=slot,packet_lag=lag,index_base=base,matched_labels=total,index_in_bounds=good,fraction=good/total if total else ''))
  # Only a candidate association: do not claim spatial truth from bounds/correlation.
  for r,ix,ok in zip(rows,idx,near):
   if not ok: continue
   mask=int(r['matched_mask'],0)
   for led in range(17):
    if mask&(1<<led):
     j=int(r[f'blob_{led}'])
     associations.append(dict(host_us=r['host_us'],frame_index=r['frame_index'],camera=cam,led=led,semantic_blob=j,nearest_packet=int(ix+1),packet_dt_us=int(times[ix]-int(r['host_us'])),section_count=int(counts[ix,cam]),in_bounds_zero_based=int(0<=j<counts[ix,cam]),candidate_box=str(blobs[ix][cam][j]) if 0<=j<counts[ix,cam] else ''))
 writecsv(out/'if8-label-index-tests.csv',matches); writecsv(out/'if8-count-correlations.csv',correlations); writecsv(out/'if8-candidate-label-boxes.csv',associations)
 # Compact coordinate-candidate view: labelled inferred records, NOT photographs of LEDs.
 from PIL import Image, ImageDraw
 frames=readcsv(out/'optical-frame-summary.csv')
 selected=[]
 for value in [4,5,6,10,12,13,14,255]:
  eligible=[r for r in frames if r['phase']=='2' and int(r['led0'])==value and abs(times[int(np.argmin(abs(times-int(r['host_us']))))]-int(r['host_us']))<30000]
  # Highest observed union is illustrative, not representative prevalence.
  if eligible: selected.append(max(eligible,key=lambda r:int(r['union_count'])))
 sheet=Image.new('RGB',(4*254,len(selected)*280),'#111111'); draw=ImageDraw.Draw(sheet)
 for row,frame in enumerate(selected):
  t=int(frame['host_us']); ix=int(np.argmin(abs(times-t)))
  draw.text((5,row*280),f"led0={int(frame['led0']):02x}, frame {frame['frame_index']}, max-union example={frame['union_count']}; candidate coordinates",fill='white')
  for cam in range(4):
   xoff=cam*254; yoff=row*280+25
   draw.rectangle((xoff,yoff,xoff+253,yoff+253),outline='#555555')
   for box in blobs[ix][cam]:
    x0,x1,y0,y1=box; draw.rectangle((xoff+x0/2,yoff+y0/2,xoff+x1/2,yoff+y1/2),outline='#666666')
   labels=next((r for r in gt if r['frame_index']==frame['frame_index'] and int(r['camera'])==cam),{})
   mask=int(labels.get('matched_mask','0'),0)
   for led in range(17):
    if mask&(1<<led):
     j=int(labels[f'blob_{led}'])
     if 0<=j<len(blobs[ix][cam]):
      x0,x1,y0,y1=blobs[ix][cam][j]
      color=['#ffcc66','#66ccff','#ff88bb','#aaff88'][cam]
      draw.rectangle((xoff+x0/2,yoff+y0/2,xoff+x1/2,yoff+y1/2),outline=color,width=2)
      draw.text((xoff+(x0+x1)/4,yoff+(y0+y1)/4),str(led),fill=color)
 sheet.save(out/'if8-candidate-labels.png')
 # Full-capture stamp vs host fit and common VI counter/timestamp agreement.
 slope,offset=np.polyfit(stamps-stamps[0],times-times[0],1); residual=(times-times[0])-(slope*(stamps-stamps[0])+offset)
 vi=readcsv(args.capture/'camera_frames.csv'); exact=[]
 cmap={int(v):i for i,v in enumerate(counters)}
 for r in vi:
  seq=int(r['sequence_id']); same=cmap.get(seq); near=int(np.argmin(abs(times-int(r['host_us']))))
  exact.append(dict(filename=r['filename'],vi_sequence=seq,vi_vts=r['vts_us'],same_counter_packet=same+1 if same is not None else '',same_counter_vts=int(stamps[same]) if same is not None else '',same_counter_host_dt_us=int(times[same]-int(r['host_us'])) if same is not None else '',vts_delta_us=int(stamps[same])-int(r['vts_us']) if same is not None else '',nearest_packet=near+1,nearest_counter=int(counters[near]),nearest_vts_delta_us=int(stamps[near])-int(r['vts_us'])))
 writecsv(out/'vi-if8-counter-alignment.csv',exact)
 summary=dict(packets=len(times),section_count_distributions=[dict(c) for c in slots],nonzero_after_count={str(k):v for k,v in nonzero_tails.items()},record_types=dict(types),record_validation=dict(audit),counter_missing=int(np.maximum(differences-1,0).sum()),counter_gaps=gaps,device_stamp_host_fit=dict(slope=float(slope),rms_us=float(np.sqrt(np.mean(residual**2))),max_abs_us=float(abs(residual).max())),same_counter_vi_rows=sum(r['same_counter_packet']!='' for r in exact),vts_delta_distribution=dict(Counter(r['vts_delta_us'] for r in exact if r['vts_delta_us']!='')))
 (out/'if8-structure-validation.json').write_text(json.dumps(summary,indent=2)); print(json.dumps(summary,indent=2))
 # Pattern count distributions and semantic correlations after observable sections corrected.
 summaryrows=[]
 for key in sorted(set((r['phase'],r['led0']) for r in packetrows)):
  inds=[i for i,r in enumerate(packetrows) if (r['phase'],r['led0'])==key]
  summaryrows.append(dict(phase=key[0],led0=key[1],packets=len(inds),**{f'section{k}_mean_count':float(counts[inds,k].mean()) for k in range(4)},**{f'section{k}_count_distribution':json.dumps(Counter(map(int,counts[inds,k]))) for k in range(4)}))
 writecsv(out/'if8-section-summary.csv',summaryrows)

if __name__=='__main__': main()
