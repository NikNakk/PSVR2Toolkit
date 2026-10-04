#!/usr/bin/env python3
"""Test neutral IF8-coordinate to VI-lane projections against shifted controls.
Scores are byte contrast, not validated LED intensities or decoded pixels.
"""
import argparse,struct
from collections import defaultdict
from pathlib import Path
import numpy as np
from analyze_psvr2_led_detector_usb import iter_records
from analyze_sony_optical_capture import readcsv,writecsv,Index


def main():
 ap=argparse.ArgumentParser();ap.add_argument('capture',type=Path);ap.add_argument('--out',type=Path,default=Path('analysis/sony-oracle-20261004'));ap.add_argument('--controller-side',choices=['L','R']);args=ap.parse_args()
 vi=readcsv(args.capture/'camera_frames.csv'); seqs={int(r['sequence_id']) for r in vi}; records={}
 for r in iter_records(args.capture/'usb-if8-led-detector.bin'):
  seq=struct.unpack_from('<I',r.payload,20)[0]
  if seq in seqs: records[seq]=r.payload
 gt=readcsv(args.capture/'sony_led_ground_truth.csv');
 if args.controller_side is None and len({r['side'] for r in gt})>1: raise ValueError('Multiple semantic controller sides: specify --controller-side')
 gt=[r for r in gt if args.controller_side is None or r['side']==args.controller_side]; gi=[Index([r for r in gt if int(r['camera'])==cam]) for cam in range(4)]
 acc=defaultdict(list); sampled=set()
 # Test full-height and stacked half-height; each quadrant additionally tests half-width.
 projections=[('half_x_full_y',.5,1,0,0),('half_xy_top',.5,.5,0,0),('half_xy_bottom',.5,.5,0,254)]
 projections += [(f'quarter_x_half_y_{x}_{y}',.25,.5,x,y) for x in [0,127] for y in [0,254]]
 for row in vi:
  seq=int(row['sequence_id'])
  if seq in sampled:continue
  sampled.add(seq);raw=(args.capture/row['filename']).read_bytes();arr=np.frombuffer(raw,dtype=np.uint8,offset=256).reshape(508,2048)[:,:2032].reshape(508,254,8).astype(float)
  payload=records[seq]
  for cam in range(4):
   label,dt=gi[cam].nearest(int(row['host_us']),30000)
   if not label:continue
   mask=int(label['matched_mask'],0); n=struct.unpack_from('<I',payload,64+cam*9220)[0]
   for led in range(17):
    if not(mask&(1<<led)):continue
    j=int(label[f'blob_{led}'])
    if j>=n:continue
    x0,x1,y0,y1=struct.unpack_from('<HHHH',payload,64+cam*9220+4+j*36+4)
    for name,sx,sy,ox,oy in projections:
     x=int((x0+x1)/2*sx+ox);y=int((y0+y1)/2*sy+oy)
     if not(2<=x<252 and 2<=y<506):continue
     local=arr[y-2:y+3,x-2:x+3,:].max(axis=(0,1))
     cx=2+(x+73)%250; cy=2+(y+83)%504
     control=arr[cy-2:cy+3,cx-2:cx+3,:].max(axis=(0,1))
     for lane in range(8):acc[(cam,lane,name)].append(float(local[lane]-control[lane]))
 writecsv(args.out/'vi-if8-coordinate-tests.csv',[dict(camera=cam,lane=lane,projection=name,label_samples=len(v),mean_byte_peak_contrast=float(np.mean(v)),median_byte_peak_contrast=float(np.median(v)),positive_fraction=float(np.mean(np.array(v)>0))) for (cam,lane,name),v in sorted(acc.items())])
 print('Unique VI frames tested:',len(sampled),'projection/lane/camera groups:',len(acc))

if __name__=='__main__':main()
