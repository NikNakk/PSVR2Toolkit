#!/usr/bin/env python3
"""Reconstruct experimental intervals from poses, labels and wire sensor candidates.
Candidate sensor fields are selected by measured correlation, not private layouts.
"""
import argparse,json,math,struct
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
from analyze_sony_sense_etw_pcap import hid_reports
from analyze_sony_optical_capture import readcsv,writecsv,Index


def main():
 ap=argparse.ArgumentParser();ap.add_argument('capture',type=Path);ap.add_argument('bluetooth',type=Path);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--handle',type=int,required=True);args=ap.parse_args();out=args.out
 c=json.loads((out/'capture-inventory.json').read_text());origin=c['qpc_origin_us'];qpc=lambda t:origin+(t-c['unix_origin_us']-c['clock_offset_us'])/c['clock_slope']
 poses=readcsv(args.capture/'poses.csv');rp=Index([r for r in poses if r['device']=='R']);hp=Index([r for r in poses if r['device']=='HMD']);raw=[];times=[]
 for ts,typ,h,cid,r in hid_reports(args.bluetooth):
  if typ==0xa1 and h==args.handle:raw.append(r);times.append(qpc(ts))
 t=np.array(times);nearest=np.searchsorted(rp.t,t);nearest=np.clip(nearest,1,len(rp.t)-1);nearest=np.where(abs(rp.t[nearest-1]-t)<abs(rp.t[nearest]-t),nearest-1,nearest)
 av=np.array([math.sqrt(sum(float(rp.rows[i][k])**2 for k in ['avx','avy','avz'])) for i in nearest]);good=abs(rp.t[nearest]-t)<10000
 tests=[];proxies={}
 for off in range(12,65):
  vals=np.array([struct.unpack_from('<hhh',r,off) for r in raw],dtype=float);m=np.linalg.norm(vals,axis=1);proxies[off]=m
  corr=float(np.corrcoef(m[good],av[good])[0,1]) if np.std(m[good]) and np.std(av[good]) else 0
  tests.append(dict(offset=off,correlation_with_published_angular_speed=corr,samples=int(good.sum()),median_raw_magnitude=float(np.median(m)),p95_raw_magnitude=float(np.percentile(m,95))))
 tests.sort(key=lambda r:r['correlation_with_published_angular_speed'],reverse=True);writecsv(out/'a131-rotation-field-tests.csv',tests);best=tests[0];m=proxies[best['offset']]
 # One-second summaries preserve ambiguity rather than hard-labeling stillness.
 fs=[r for r in readcsv(out/'capture-optical-frames.csv') if r['side']=='R'];fi=Index(fs)
 seconds=[]
 for sec in range(math.floor((t.min()-origin)/1e6),math.ceil((t.max()-origin)/1e6)):
  lo=origin+sec*1e6;hi=lo+1e6;sel=(t>=lo)&(t<hi);row=dict(start_relative_s=sec,input_reports=int(sel.sum()),rotation_proxy_offset=best['offset'],rotation_proxy_median=float(np.median(m[sel])) if sel.any() else '',published_angular_speed_median=float(np.median(av[sel])) if sel.any() else '')
  for side,idx in [('R',rp),('HMD',hp)]:
   pp=idx.rows[int(np.searchsorted(idx.t,lo)):int(np.searchsorted(idx.t,hi))];valid=[r for r in pp if r['pose_valid']=='1'];xyz=np.array([[float(r[k]) for k in ['px','py','pz']] for r in valid]);qs=np.array([[float(r[k]) for k in ['qw','qx','qy','qz']] for r in valid]);qs=qs/np.linalg.norm(qs,axis=1,keepdims=True) if len(qs) else qs
   row[side+'_pose_samples']=len(pp);row[side+'_valid_fraction']=len(valid)/len(pp) if pp else ''
   row[side+'_position_range_m']=float(np.linalg.norm(np.ptp(xyz,axis=0))) if len(xyz) else ''
   row[side+'_orientation_range_deg']=float(2*np.degrees(np.arccos(np.clip(abs(qs@qs[0]),0,1))).max()) if len(qs) else ''
  ff=fi.rows[int(np.searchsorted(fi.t,lo)):int(np.searchsorted(fi.t,hi))];row['optical_frames']=len(ff);row['matched_union_mean']=float(np.mean([int(r['union_count']) for r in ff])) if ff else '';row['zero_match_fraction']=sum(r['union_count']=='0' for r in ff)/len(ff) if ff else ''
  seconds.append(row)
 writecsv(out/'experiment-seconds.csv',seconds)
 gaps=[];start=None
 for r in fs:
  if r['union_count']=='0' and start is None:start=r
  elif r['union_count']!='0' and start is not None:
   duration=float(r['relative_s'])-float(start['relative_s'])
   if duration>=.25:gaps.append(dict(start_relative_s=start['relative_s'],end_relative_s=r['relative_s'],duration_s=duration,end_censored=0))
   start=None
 if start:gaps.append(dict(start_relative_s=start['relative_s'],end_relative_s=fs[-1]['relative_s'],duration_s=float(fs[-1]['relative_s'])-float(start['relative_s']),end_censored=1))
 writecsv(out/'optical-zero-match-intervals.csv',gaps)
 # Pose-freeze does not mean controller disconnection; retain input freshness checks.
 tail=[r for r,timestamp in zip(raw,t) if (timestamp-origin)/1e6>=220]
 (out/'input-tail-freshness.json').write_text(json.dumps(dict(reports=len(tail),unique_full_reports=len(set(tail)),unique_rotation_triplets=len(set(struct.unpack_from('<hhh',r,int(best['offset'])) for r in tail))),indent=2))
 # Join X candidate marker intervals onto the semantic-row timeline after full analysis.
 marks=[r for r in readcsv(out/'button-markers.csv') if int(r['acl_handle'])==args.handle and r['field']=='byte9' and r['bit']=='1']
 for n,r in enumerate(marks,1):r['marker_number']=n;r['host_us']=r['press_qpc_us']
 mi=Index(marks);event_index=Index(readcsv(args.capture/'events.csv'));timeline=readcsv(out/'unified-timeline.csv')
 for r in timeline:
  prev=mi.previous(int(r['host_us']));near,dt=mi.nearest(int(r['host_us']));ev,evdt=event_index.nearest(int(r['host_us']));r['event_side']=ev.get('side','');r.update(previous_x_marker=prev.get('marker_number',''),nearest_x_marker=near.get('marker_number',''),x_marker_dt_us=dt)
 writecsv(out/'unified-timeline-with-markers.csv',timeline)
 if len(marks)==6 and len(gaps)>=4:
  mk=[float(r['press_relative_s']) for r in marks]
  intervals=[('quiet_hold_1',mk[0]+.5,mk[1]-.5),('quiet_hold_2',mk[1]+.5,float(gaps[1]['start_relative_s'])),
             ('first_optical_gap',float(gaps[1]['start_relative_s']),float(gaps[1]['end_relative_s'])),
             ('quiet_hold_after_gap',float(gaps[1]['end_relative_s'])+1,mk[4]),
             ('free_movement_1',mk[4]+.2,float(gaps[2]['start_relative_s'])),
             ('second_optical_gap',float(gaps[2]['start_relative_s']),float(gaps[2]['end_relative_s'])),
             ('free_movement_2',float(gaps[2]['end_relative_s']),mk[5]+.3),
             ('later_settling',mk[5]+.3,float(gaps[3]['start_relative_s'])),
             ('probable_teardown_or_unobserved_stillness',float(gaps[3]['start_relative_s']),float(gaps[3]['end_relative_s']))]
  interval_rows=[]
  for label,lo,hi in intervals:
   rr=[r for r in seconds if lo<=r['start_relative_s']<hi]
   result=dict(interval=label,start_relative_s=lo,end_relative_s=hi,summary_seconds=len(rr))
   for key in ['R_position_range_m','R_orientation_range_deg','HMD_position_range_m','rotation_proxy_median','matched_union_mean','zero_match_fraction']:
    vv=[float(r[key]) for r in rr if r[key]!='']
    result[key+'_median']=float(np.median(vv)) if vv else ''
   interval_rows.append(result)
  writecsv(out/'reconstructed-intervals.csv',interval_rows)
 # Compact evidence plot: published movement vs independent candidate rotational signal.
 image=Image.new('RGB',(1100,800),'#161616');draw=ImageDraw.Draw(image);left=65;width=1000;span=max(s['start_relative_s'] for s in seconds)+1
 panels=[('R_position_range_m',.3,'R position range per second (m)'),('rotation_proxy_median',3000,f'Raw rotational candidate norm (offset {best["offset"]}; correlation {best["correlation_with_published_angular_speed"]:.3f})'),('matched_union_mean',17,'R semantic matched union (mean per second)')]
 for k,(field,scale,title) in enumerate(panels):
  top=35+k*235;bottom=top+190;draw.text((left,top-20),title,fill='white');draw.text((3,top),f'{scale:g}',fill='#aaaaaa');draw.text((25,bottom-10),'0',fill='#aaaaaa');draw.line((left,bottom,left+width,bottom),fill='#777777');points=[]
  for r in seconds:
   if r.get(field,'')=='':continue
   x=left+width*r['start_relative_s']/span;y=bottom-180*min(float(r[field])/scale,1);points.append((x,y))
  if len(points)>1:draw.line(points,fill=['#66ccff','#ffcc66','#99dd99'][k],width=2)
  for r in marks:
   x=left+width*float(r['press_relative_s'])/span;draw.line((x,top,x,bottom),fill='#774477');draw.text((x+2,top+3),str(r['marker_number']),fill='#eeaaff')
  for gap in gaps:
   x0=left+width*float(gap['start_relative_s'])/span;x1=left+width*float(gap['end_relative_s'])/span;draw.line((x0,bottom+5,x1,bottom+5),fill='#ff7777',width=4)
 for sec in range(0,int(span)+1,20):draw.text((left+width*sec/span,735),str(sec),fill='white')
 draw.text((65,770),'Seconds from capture clock start; purple lines: candidate X markers; red bars: zero semantic matches. Position graph clips at 0.3 m.',fill='#dddddd')
 image.save(out/'experiment-timeline.png');summary=dict(rotation_candidate=best,zero_match_intervals=gaps,x_markers=len(marks),note='Sensor triplet selected empirically by correlation; axis meanings and scale unassigned. Marker meaning from user recollection remains approximate.')
 (out/'experiment-reconstruction.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
