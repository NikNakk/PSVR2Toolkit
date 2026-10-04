#!/usr/bin/env python3
"""Whole-capture behavioural-oracle analysis; no private tracker layouts.
Requires numpy and Pillow. Streams IF8 and gzip pcapng. Raw inputs stay untouched.
All associations are host-time associations, not asserted optical frame identity.
"""
from __future__ import annotations
import argparse, csv, hashlib, json, math, struct, zlib
from collections import Counter, defaultdict
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from analyze_psvr2_led_detector_usb import iter_records, Record, timestamp_candidates
from analyze_sony_sense_etw_pcap import reports, stream_enhanced_packets
from extract_psvr2_vi_lanes import parse_header, PACKET_SIZE
from analyze_psvr2_led_frame_changes import components


def readcsv(p):
    with p.open(newline='', encoding='utf-8-sig') as f:
        return list(csv.DictReader(f))


def writecsv(p, rows, fields=None):
    rows = list(rows)
    fields = fields or list(dict.fromkeys(k for r in rows for k in r))
    with p.open('w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader(); w.writerows(rows)


def profile(name, times, **extra):
    t = np.asarray(times, dtype=np.int64); dt = np.diff(t)
    positive = dt[dt > 0]
    return dict(stream=name, count=len(t), first_qpc_us=int(t.min()) if len(t) else '',
                last_qpc_us=int(t.max()) if len(t) else '',
                duration_s=float(np.ptp(t)/1e6) if len(t) else 0,
                rate_hz=float((len(t)-1)*1e6/np.ptp(t)) if len(t)>1 and np.ptp(t) else 0,
                median_interval_us=float(np.median(positive)) if len(positive) else '',
                max_interval_us=int(dt.max()) if len(dt) else '',
                nonpositive_intervals=int((dt<=0).sum()), **extra)


class Index:
    def __init__(self, rows, field='host_us'):
        self.rows=sorted(rows,key=lambda r: float(r[field])); self.t=np.array([float(r[field]) for r in self.rows])
    def nearest(self, t, limit=math.inf):
        if not len(self.t): return {}, math.inf
        i=int(np.searchsorted(self.t,t)); i=min(max(i,0),len(self.t)-1)
        if i and abs(self.t[i-1]-t)<abs(self.t[i]-t): i-=1
        d=float(self.t[i]-t)
        return (self.rows[i],d) if abs(d)<=limit else ({},d)
    def previous(self,t):
        i=int(np.searchsorted(self.t,t,side='right'))-1
        return self.rows[i] if i>=0 else {}


def write_source_inventory(capture, bluetooth, out):
    """Stream source hashes and derive phase episodes from the saved wire runs."""
    manifest = []
    for path in sorted(capture.iterdir()):
        if path.suffix not in ('.csv', '.vi', '.bin'):
            continue
        digest = hashlib.sha256()
        with path.open('rb') as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b''):
                digest.update(chunk)
        manifest.append(dict(filename=path.name, bytes=path.stat().st_size, sha256=digest.hexdigest()))
    digest = hashlib.sha256()
    with bluetooth.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            digest.update(chunk)
    manifest.append(dict(filename=str(bluetooth), bytes=bluetooth.stat().st_size, sha256=digest.hexdigest()))
    writecsv(out/'source-manifest.csv', manifest)
    runs = readcsv(out/'a231-runs.csv')
    origin = float(runs[0]['start_qpc_us'])
    episodes = []
    for r in runs:
        if not episodes or episodes[-1]['phase'] != int(r['phase']):
            episodes.append(dict(phase=int(r['phase']), start_qpc_us=float(r['start_qpc_us']),
                                 end_qpc_us=float(r['end_qpc_us']), reports=int(r['reports'])))
        else:
            episodes[-1]['end_qpc_us'] = float(r['end_qpc_us'])
            episodes[-1]['reports'] += int(r['reports'])
    for e in episodes:
        e.update(start_relative_s=(e['start_qpc_us']-origin)/1e6,
                 duration_s=(e['end_qpc_us']-e['start_qpc_us'])/1e6)
    writecsv(out/'phase-episodes.csv', episodes)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture',type=Path); ap.add_argument('bluetooth',type=Path)
    ap.add_argument('--out',type=Path,default=Path('analysis/sony-oracle-20261004'))
    ap.add_argument('--vrserver',type=Path)
    ap.add_argument('--correct-reversed-ground-truth-side',action='store_true',help='Explicit opt-in for pre-fix capture')
    args=ap.parse_args(); out=args.out; out.mkdir(parents=True,exist_ok=True)
    source=args.capture
    data={n:readcsv(source/(n+'.csv')) for n in ['events','clock_sync','sony_led_ground_truth','poses','camera_frames']}
    sync=data['clock_sync']; x=np.array([int(r['qpc_us']) for r in sync],dtype=np.int64)
    y=np.array([int(r['unix_us']) for r in sync],dtype=np.int64)
    x0,y0=int(x[0]),int(y[0]); slope,offset=np.polyfit(x-x0,y-y0,1)
    residual=(y-y0)-(slope*(x-x0)+offset)
    utc=lambda t: y0+offset+slope*(t-x0)
    qpc=lambda t: x0+(t-y0-offset)/slope
    clock=dict(qpc_origin_us=x0,unix_origin_us=y0,slope=float(slope),offset_us=float(offset),
               rms_us=float(np.sqrt(np.mean(residual**2))),max_abs_us=float(abs(residual).max()),
               residual_p95_abs_us=float(np.percentile(abs(residual),95)),pairs=len(x))
    writecsv(out/'clock-residuals.csv',[dict(**r,residual_us=float(e)) for r,e in zip(sync,residual)])
    a=list(reports(args.bluetooth))
    for r in a: r['host_us']=qpc(r['capture_ts_us'])
    ai=Index(a); writecsv(out/'a231.csv',a)
    audit=Counter(); input_times=[]; output_times=[]; bad=Counter(); candidates=Counter(); a1seq=[]
    for ts,packet in stream_enhanced_packets(args.bluetooth,audit):
        for typ,length in [(0xa1,78),(0xa2,78)]:
            start=0
            while True:
                pos=packet.find(bytes([typ,0x31]),start)
                if pos<0: break
                start=pos+1; candidates[hex(typ)]+=1
                if pos+length+1>len(packet): bad[hex(typ)+'_short_candidate']+=1; continue
                r=packet[pos+1:pos+1+length]
                if zlib.crc32(bytes([typ])+r[:74])&0xffffffff != int.from_bytes(r[74:78],'little'):
                    bad[hex(typ)+'_crc_rejected_candidate']+=1; continue
                if typ==0xa1: input_times.append(qpc(ts)); a1seq.append(r[1])
                else: output_times.append(qpc(ts))
    audit.update(candidates); audit.update(bad); audit['valid_a131']=len(input_times); audit['valid_a231']=len(output_times)
    # Runs hold settings until next observed command, including timing/sequence changes.
    fields=['phase','sequence','period','cycle_position','cycle_length','led0','led1','led2','led3']
    runs=[]
    for i,r in enumerate(a):
        if not runs or any(r[k]!=runs[-1][k] for k in fields):
            if runs: runs[-1]['end_qpc_us']=r['host_us']
            runs.append(dict(run_id=len(runs)+1,start_qpc_us=r['host_us'],end_qpc_us=r['host_us'],reports=0,**{k:r[k] for k in fields}))
        runs[-1]['reports']+=1; runs[-1]['last_report_qpc_us']=r['host_us']
        if i==len(a)-1: runs[-1]['end_qpc_us']=r['host_us']
    ri=Index(runs,'start_qpc_us')
    def setting(t):
        r=ri.previous(t)
        return r if r and t<=a[-1]['host_us'] else {}
    poses=data['poses']; right=[r for r in poses if r['device']=='R']; pi=Index(right)
    vi=Index(data['camera_frames']); events=Index(data['events'])
    profiles=[profile(n,[int(r['host_us']) for r in rows]) for n,rows in data.items() if n!='clock_sync']
    profiles.append(profile('clock_sync',x)); profiles.append(profile('A2/31',output_times)); profiles.append(profile('A1/31',input_times))
    for dev in sorted(set(r['device'] for r in poses)):
        profiles.append(profile('pose_'+dev,[int(r['host_us']) for r in poses if r['device']==dev]))
    gt=data['sony_led_ground_truth']; corrected=0
    for r in gt:
        if args.correct_reversed_ground_truth_side:
            r['original_side']=r['side']; r['side']={'L':'R','R':'L'}[r['side']]; corrected+=1
        r['assigned_int']=int(r['assigned_mask'],0); r['matched_int']=int(r['matched_mask'],0)
    gti=Index(gt)
    # Stream complete IF8: per-offset alphabet/change count, block activity, and compact prefixes only.
    if8=[]; prefixes=[]; sizes=Counter(); ifaces=Counter(); previous=None; previous_t=None
    changes=seen=minimum=maximum=None; phase_acc={}; sums={}; sqs={}; ns=Counter(); hashes=Counter()
    block_rows=[]; counter_records=[]
    for number,r in enumerate(iter_records(source/'usb-if8-led-detector.bin'),1):
        b=np.frombuffer(r.payload,dtype=np.uint8); sizes[len(b)]+=1; ifaces[(r.interface,r.pipe)]+=1
        if changes is None:
            changes=np.zeros(len(b),dtype=np.int64); seen=np.zeros((len(b),256),dtype=bool)
            minimum=b.copy(); maximum=b.copy()
        if len(b)!=len(changes): raise ValueError('variable IF8 size; full-byte comparison invalid')
        seen[np.arange(len(b)),b]=True; minimum=np.minimum(minimum,b); maximum=np.maximum(maximum,b)
        if previous is not None: changes+=b!=previous
        s=setting(r.host_us); key=(s.get('phase',-1),s.get('led0',-1))
        if key not in sums: sums[key]=np.zeros(len(b)); sqs[key]=np.zeros(len(b))
        sums[key]+=b; sqs[key]+=b.astype(float)**2; ns[key]+=1
        digest=hashlib.sha256(r.payload).hexdigest(); hashes[digest]+=1
        row=dict(packet_index=number,host_us=r.host_us,unix_us=round(utc(r.host_us)),size=len(b),
                 phase=key[0],led0=key[1],run_id=s.get('run_id',''),nonzero_bytes=int(np.count_nonzero(b)),
                 changed_bytes=int(np.count_nonzero(b!=previous)) if previous is not None else '',
                 prefix_hex=r.payload[:64].hex(),sha256=digest)
        # Neutral fixed-size windows: grouping is explicitly exploratory.
        for k in range(4):
            start=64+k*9220; chunk=b[start:start+9220]
            row[f'window{k}_nonzero']=int(np.count_nonzero(chunk))
            row[f'window{k}_changed']=int(np.count_nonzero(chunk!=previous[start:start+9220])) if previous is not None else ''
        if8.append(row); prefixes.append(Record(r.host_us,r.interface,r.pipe,r.payload[:516]))
        previous=b.copy(); previous_t=r.host_us
    ii=Index(if8); writecsv(out/'if8-packets.csv',if8)
    profiles.append(profile('IF8',[r['host_us'] for r in if8],payload_sizes=json.dumps(dict(sizes))))
    bytestats=[dict(offset=i,unique=int(seen[i].sum()),changes=int(changes[i]),change_fraction=float(changes[i]/max(len(if8)-1,1)),min=int(minimum[i]),max=int(maximum[i])) for i in range(len(changes))]
    writecsv(out/'if8-byte-stats.csv',bytestats)
    tc=timestamp_candidates(prefixes,len(prefixes[0].payload)-4); writecsv(out/'if8-counter-candidates.csv',tc)
    # Header aligned u16/u32 deltas across the entire stream, never asserted timestamps.
    headerstats=[]
    for width in [2,4]:
        for off in range(0,80,width):
            vals=np.array([int.from_bytes(r.payload[off:off+width],'little') for r in prefixes],dtype=np.int64)
            ds=(np.diff(vals)%(1<<(width*8)))
            headerstats.append(dict(offset=off,width=width,unique=len(set(vals)),first=int(vals[0]),last=int(vals[-1]),delta_modes=json.dumps(Counter(map(int,ds)).most_common(5))))
    writecsv(out/'if8-header-summary.csv',headerstats)
    # Between-setting byte discriminability, and within-setting variability controls.
    broadkeys=sorted(k for k in sums if k[0]==2)
    means=np.stack([sums[k]/ns[k] for k in broadkeys]); variances=np.stack([sqs[k]/ns[k]-(sums[k]/ns[k])**2 for k in broadkeys])
    span=np.ptp(means,axis=0); noise=np.sqrt(np.maximum(variances.mean(axis=0),0))
    order=np.argsort(span/(noise+1))[::-1][:128]
    writecsv(out/'if8-pattern-offsets.csv',[dict(offset=int(i),between_mean_range=float(span[i]),within_sd=float(noise[i]),ratio=float(span[i]/(noise[i]+1)),**{f'mean_{k[1]:02x}':float(means[j,i]) for j,k in enumerate(broadkeys)}) for i in order])
    if8summary=[]
    for key,n in sorted(ns.items()):
        rows=[r for r in if8 if (r['phase'],r['led0'])==key]
        if8summary.append(dict(phase=key[0],led0=key[1],packets=n,mean_nonzero=float(np.mean([r['nonzero_bytes'] for r in rows])),mean_changed=float(np.mean([r['changed_bytes'] for r in rows if r['changed_bytes']!=''])),**{f'window{k}_mean_nonzero':float(np.mean([r[f'window{k}_nonzero'] for r in rows])) for k in range(4)}))
    writecsv(out/'if8-summary.csv',if8summary)
    # Full GT timeline, row grain = one semantic camera result. Joins include signed distances.
    timeline=[]; groups=defaultdict(list); run_gt=defaultdict(list); framegroups=defaultdict(list)
    for r in gt:
        t=int(r['host_us']); s=setting(t); p,pd=pi.nearest(t,50000); v,vd=vi.nearest(t); u,ud=ii.nearest(t); e,ed=events.nearest(t)
        row={k:v for k,v in r.items() if k not in ['assigned_int','matched_int']}
        row.update(unix_us=round(utc(t)),run_id=s.get('run_id',''),**{k:s.get(k,'') for k in fields},
                   event_id=e.get('event_id',''),event_kind=e.get('kind',''),event_detail=e.get('detail',''),event_dt_us=ed,
                   pose_dt_us=pd,pose_valid=p.get('pose_valid',''),tracking_result=p.get('tracking_result',''),
                   **{k:p.get(k,'') for k in ['px','py','pz','qw','qx','qy','qz','pose_time_offset_s']},
                   nearest_vi=v.get('filename',''),vi_dt_us=vd,vi_sequence=v.get('sequence_id',''),
                   nearest_if8=u.get('packet_index',''),if8_dt_us=ud)
        timeline.append(row); groups[(s.get('phase',-1),s.get('led0',-1),int(r['camera']))].append(r)
        run_gt[s.get('run_id',0)].append(r); framegroups[int(r['frame_index'])].append(r)
    writecsv(out/'unified-timeline.csv',timeline)
    writecsv(out/'stream-summary.csv',profiles)
    def gstats(rows):
        matched=[r['matched_int'] for r in rows]; assigned=[r['assigned_int'] for r in rows]
        return dict(rows=len(rows),mean_matched=float(np.mean([v.bit_count() for v in matched])) if rows else 0,
                    matched_mask_distribution=json.dumps(Counter(hex(v) for v in matched)),assigned_mask_distribution=json.dumps(Counter(hex(v) for v in assigned)),
                    **{f'led_{i}_matched_fraction':sum(bool(v&(1<<i)) for v in matched)/len(rows) if rows else 0 for i in range(17)})
    ground=[dict(phase=k[0],led0=k[1],camera=k[2],**gstats(rows)) for k,rows in sorted(groups.items())]
    writecsv(out/'ground-truth-summary.csv',ground)
    frame_rows=[]
    for frame,rows in sorted(framegroups.items()):
        t=min(int(r['host_us']) for r in rows); s=setting(t); mask=0
        for r in rows: mask|=r['matched_int']
        p,pd=pi.nearest(t,50000)
        frame_rows.append(dict(host_us=t,frame_index=frame,phase=s.get('phase',-1),led0=s.get('led0',-1),
                               run_id=s.get('run_id',0),camera_rows=len(rows),union_mask=hex(mask),union_count=mask.bit_count(),
                               pose_valid=int(p['pose_valid']) if p else -1,tracking_result=p.get('tracking_result',''),pose_dt_us=pd))
    fi=Index(frame_rows); writecsv(out/'optical-frame-summary.csv',frame_rows)
    patterns=[]
    for key in sorted(set((r['phase'],r['led0']) for r in frame_rows)):
        fs=[r for r in frame_rows if (r['phase'],r['led0'])==key]; known=[r for r in fs if r['pose_valid']>=0]
        adjacent=[(f,g) for f,g in zip(frame_rows,frame_rows[1:]) if (f['phase'],f['led0'])==key and g['host_us']-f['host_us']<50000 and f['pose_valid']>=0 and g['pose_valid']>=0]
        lost=[(f,g) for f,g in adjacent if not f['pose_valid']]; tracked=[(f,g) for f,g in adjacent if f['pose_valid']]
        patterns.append(dict(phase=key[0],led0=key[1],optical_frames=len(fs),known_pose_frames=len(known),
                             union_mean=float(np.mean([r['union_count'] for r in fs])),union_distribution=json.dumps(Counter(r['union_mask'] for r in fs)),
                             valid_fraction=sum(r['pose_valid'] for r in known)/len(known) if known else '',
                             acquisition_trials=len(lost),acquisitions=sum(g['pose_valid'] for f,g in lost),
                             acquisition_probability=sum(g['pose_valid'] for f,g in lost)/len(lost) if lost else '',
                             retention_trials=len(tracked),retentions=sum(g['pose_valid'] for f,g in tracked),
                             retention_probability=sum(g['pose_valid'] for f,g in tracked)/len(tracked) if tracked else ''))
    writecsv(out/'led-pattern-summary.csv',patterns)
    for run in runs:
        rows=run_gt[run['run_id']]; run.update(duration_s=(run['end_qpc_us']-run['start_qpc_us'])/1e6,**gstats(rows))
        ps=[p for p in right if run['start_qpc_us']<=int(p['host_us'])<run['end_qpc_us']]
        run['pose_samples']=len(ps); run['pose_valid_fraction']=sum(int(p['pose_valid']) for p in ps)/len(ps) if ps else ''
    writecsv(out/'a231-runs.csv',runs)
    # Transitions: fixed windows and latency annotations, no causal claims.
    transitions=[]
    for prev,run in zip(runs,runs[1:]):
        t=run['start_qpc_us']; row=dict(host_us=t,from_phase=prev['phase'],to_phase=run['phase'],from_led0=prev['led0'],to_led0=run['led0'],run_id=run['run_id'])
        for label,lo,hi in [('pre',-100000,0),('post',0,100000)]:
            fs=[f for f in frame_rows if lo<=f['host_us']-t<hi]; us=[u for u in if8 if lo<=u['host_us']-t<hi]
            row[label+'_frames']=len(fs); row[label+'_union_mean']=float(np.mean([f['union_count'] for f in fs])) if fs else ''
            row[label+'_valid_fraction']=float(np.mean([f['pose_valid'] for f in fs if f['pose_valid']>=0])) if any(f['pose_valid']>=0 for f in fs) else ''
            row[label+'_if8_nonzero']=float(np.mean([u['nonzero_bytes'] for u in us])) if us else ''
        transitions.append(row)
    writecsv(out/'transition-summary.csv',transitions)
    # Pose state episodes, important because pose_valid can remain true in rotation-only tracking.
    episodes=[]
    for p in sorted(right,key=lambda r:int(r['host_us'])):
        key=(p['pose_valid'],p['tracking_result'],p['connected'])
        if not episodes or key!=episodes[-1]['key']:
            if episodes: episodes[-1]['end_qpc_us']=int(p['host_us'])
            s=setting(int(p['host_us'])); episodes.append(dict(key=key,start_qpc_us=int(p['host_us']),end_qpc_us=int(p['host_us']),phase=s.get('phase',''),led0=s.get('led0',''),samples=0))
        episodes[-1]['samples']+=1; episodes[-1]['end_qpc_us']=int(p['host_us'])
    writecsv(out/'tracking-episodes.csv',[dict(start_qpc_us=e['start_qpc_us'],end_qpc_us=e['end_qpc_us'],duration_s=(e['end_qpc_us']-e['start_qpc_us'])/1e6,pose_valid=e['key'][0],tracking_result=e['key'][1],connected=e['key'][2],phase=e['phase'],led0=e['led0'],samples=e['samples']) for e in episodes])
    # Validate ALL VI records; stream one image at a time, retain only compact feature rows.
    vis=[]; seenfiles=set(); errors=[]; byevent=defaultdict(dict); lane_features=[]
    def lanes(path):
        raw=path.read_bytes()
        return np.frombuffer(raw,dtype=np.uint8,offset=256).reshape(508,2048)[:,:2032].reshape(508,254,8)
    for r in data['camera_frames']:
        p=source/r['filename']; seenfiles.add(p.name); raw=p.read_bytes(); hdr=parse_header(raw)
        for k in ['sequence_id','vts_us','camera_set','image_width','image_height','active_width','active_height']:
            if hdr[k]!=int(r[k]): errors.append(f'{p.name}: {k} mismatch')
        if len(raw)!=int(r['size']) or len(raw)!=PACKET_SIZE or hdr['packet_size']!=len(raw): errors.append(f'{p.name}: size mismatch')
        arr=lanes(p); t=int(r['host_us']); s=setting(t); f,fd=fi.nearest(t,30000)
        for lane in range(8):
            b=arr[:,:,lane]; lane_features.append(dict(host_us=t,filename=p.name,lane=lane,phase=s.get('phase',''),led0=s.get('led0',''),mean=float(b.mean()),std=float(b.std()),bright_240=int((b>=240).sum()),optical_dt_us=fd,union_count=f.get('union_count','')))
        vis.append(dict(**r,sha256=hashlib.sha256(raw).hexdigest(),**{'header_'+k:v for k,v in hdr.items()}))
        byevent[int(r['event_id'])][int(r['relative_frame'])]=r
    writecsv(out/'vi-validation.csv',vis); writecsv(out/'vi-lane-summary.csv',lane_features)
    orphan=sorted(p.name for p in source.glob('*.vi') if p.name not in seenfiles)
    # Every available pre/post pair; components are neutral byte-change regions.
    diffs=[]; regions=[]; sheets=[]
    for eid,rels in sorted(byevent.items()):
        if -1 not in rels or 1 not in rels: continue
        pre,post=rels[-1],rels[1]; t0,t1=int(pre['host_us']),int(post['host_us'])
        ar,br=lanes(source/pre['filename']),lanes(source/post['filename']); d=np.abs(br.astype(np.int16)-ar.astype(np.int16)).astype(np.uint8)
        p0,_=pi.nearest(t0,50000); p1,_=pi.nearest(t1,50000)
        motion=math.sqrt(sum((float(p1[k])-float(p0[k]))**2 for k in ['px','py','pz'])) if p0 and p1 and p0['pose_valid']=='1' and p1['pose_valid']=='1' else math.nan
        qdot=abs(sum(float(p1[k])*float(p0[k]) for k in ['qw','qx','qy','qz'])) if p0 and p1 and p0['pose_valid']=='1' and p1['pose_valid']=='1' else math.nan
        angle=math.degrees(2*math.acos(min(1,qdot))) if math.isfinite(qdot) else math.nan
        s0,s1=setting(t0),setting(t1); commands=[r for r in a if t0<=r['host_us']<=t1]
        ev=next((e for e in data['events'] if int(e['event_id'])==eid),{})
        for lane in range(8):
            regs=components(d[:,:,lane].tobytes(),24,2)
            diffs.append(dict(event_id=eid,kind=ev.get('kind',''),lane=lane,pre_qpc_us=t0,post_qpc_us=t1,
                              pre_phase=s0.get('phase',''),post_phase=s1.get('phase',''),pre_led0=s0.get('led0',''),post_led0=s1.get('led0',''),wire_commands_between=len(commands),
                              pose_translation_m=motion,pose_rotation_deg=angle,mean_abs_change=float(d[:,:,lane].mean()),changed_pixels_24=int((d[:,:,lane]>=24).sum()),regions=len(regs)))
            for rank,reg in enumerate(regs[:8]): regions.append(dict(event_id=eid,lane=lane,rank=rank+1,**reg))
        # Keep representative early, lowest-motion, and phase-transition pairs only.
        sheets.append((motion+angle/1000 if math.isfinite(motion+angle) else 1e9,eid,pre,post,s0,s1))
    writecsv(out/'vi-transition-summary.csv',diffs); writecsv(out/'vi-changed-regions.csv',regions)
    chosen=list(dict.fromkeys([v[1] for v in sheets if v[4].get('phase')!=v[5].get('phase') and v[4].get('phase') in (1,2) and v[5].get('phase') in (1,2)]+[v[1] for v in sorted(sheets) if v[4].get('phase')==2 and v[5].get('phase')==2 and v[4].get('led0')!=v[5].get('led0')][:3]))[:5]
    for _,eid,pre,post,s0,s1 in sheets:
        if eid not in chosen: continue
        ar,br=lanes(source/pre['filename']),lanes(source/post['filename']); d=np.abs(br.astype(np.int16)-ar.astype(np.int16)).astype(np.uint8)
        sheet=Image.new('RGB',(8*127,3*254+60),'#111111'); draw=ImageDraw.Draw(sheet)
        draw.text((5,5),f'Event {eid}: phase {s0.get("phase")}->{s1.get("phase")}, led0 {s0.get("led0")}->{s1.get("led0")} / pre, post, |difference| x4',fill='white')
        for lane in range(8):
            draw.text((lane*127+5,25),f'Byte lane {lane}',fill='white')
            for row,arr in enumerate([ar,br,np.minimum(d.astype(np.uint16)*4,255).astype(np.uint8)]):
                sheet.paste(Image.fromarray(arr[:,:,lane]).resize((127,254)),(lane*127,60+row*254))
        sheet.save(out/f'vi-event-{eid:03d}.png')
    # Compact overview at evenly spaced sample times; highlights sparse VI coverage/cap.
    samples=[data['camera_frames'][i] for i in np.linspace(0,len(data['camera_frames'])-1,6,dtype=int)]
    sheet=Image.new('RGB',(1016,6*274),'#111111'); draw=ImageDraw.Draw(sheet)
    for row,r in enumerate(samples):
        ar=lanes(source/r['filename']); s=setting(int(r['host_us']))
        draw.text((5,row*274),f'QPC {r["host_us"]}, phase {s.get("phase")}, led0 {s.get("led0")} / lanes 0..7',fill='white')
        for lane in range(8): sheet.paste(Image.fromarray(ar[:,:,lane]).resize((127,254)),(lane*127,row*274+20))
    sheet.save(out/'vi-contact-sheet.png')
    # IF8 packet raster is byte evidence, not an image interpretation.
    chosen_packets=np.linspace(0,len(if8)-1,6,dtype=int); raster=Image.new('RGB',(512,6*100),'#111111'); draw=ImageDraw.Draw(raster)
    for n,r in enumerate(iter_records(source/'usb-if8-led-detector.bin')):
        if n in chosen_packets:
            row=list(chosen_packets).index(n); b=np.frombuffer(r.payload,dtype=np.uint8)
            padded=np.pad(b,(0,512*73-len(b))).reshape(73,512)
            raster.paste(Image.fromarray(padded),(0,row*100+20)); s=setting(r.host_us)
            draw.text((5,row*100),f'Packet {n+1}, phase {s.get("phase")}, led0 {s.get("led0")}',fill='white')
    raster.save(out/'if8-byte-contact-sheet.png')
    log=[]
    if args.vrserver and args.vrserver.exists():
        log=[l for l in args.vrserver.read_text(errors='replace').splitlines() if 'Sony Optical Capture' in l or 'dropped frames' in l]
    validation=dict(clock=clock,bluetooth=dict(audit),corrected_ground_truth_rows=corrected,
                    ground_truth_original_sides=dict(Counter(r.get('original_side',r['side']) for r in gt)),
                    ground_truth_cameras=dict(Counter(r['camera'] for r in gt)),
                    ground_truth_frame_gaps=dict(Counter(map(int,np.diff(sorted(framegroups)))).most_common(10)),
                    ground_truth_mask_out_of_range=sum(bool((r['assigned_int']|r['matched_int'])>>17) for r in gt),
                    vi_errors=errors,orphan_vi_files=orphan,unique_vi_sequences=len(set(r['sequence_id'] for r in vis)),
                    unique_vi_payloads=len(set(r['sha256'] for r in vis)),if8_interfaces={str(k):v for k,v in ifaces.items()},
                    if8_unique_payloads=len(hashes),if8_bytes=(source/'usb-if8-led-detector.bin').stat().st_size,
                    if8_zero_offsets=int((maximum==0).sum()),if8_constant_offsets=int((minimum==maximum).sum()),
                    vi_pairs=len(sheets),representative_events=chosen,log_observations=log,
                    join_abs_us={k:dict(median=float(np.median([abs(r[k]) for r in timeline])),p95=float(np.percentile([abs(r[k]) for r in timeline],95)),max=float(max(abs(r[k]) for r in timeline))) for k in ['pose_dt_us','vi_dt_us','if8_dt_us','event_dt_us']})
    (out/'validation.json').write_text(json.dumps(validation,indent=2,default=lambda v:int(v)),encoding='utf-8')
    print(json.dumps(validation,indent=2,default=lambda v:int(v)))
    write_source_inventory(source, args.bluetooth, out)

if __name__=='__main__': main()
