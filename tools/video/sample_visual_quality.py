"""Read-only, CPU-only matched-PTS visual samples of completed strict movie outputs.

Creates faithful comparison sheets under .local. No inference, media publication or scheduling occurs.
"""
from __future__ import annotations

import argparse
from fractions import Fraction
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import subprocess

import numpy as np
from PIL import Image, ImageDraw

_spec=importlib.util.spec_from_file_location("visual_video",Path(__file__).with_name("upscale_movies.py"))
video=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(video)
ROOT=Path(__file__).resolve().parents[2]
FORMAT="mc3-video-visual-samples-v1"


def safe_movie(value):
    if not isinstance(value,str) or not value or "\\" in value or ":" in value:
        raise ValueError("movie must be a normalized source-relative name")
    p=PurePosixPath(value)
    if p.is_absolute() or any(x in (".","..","") for x in value.split("/")) or p.suffix!=".bik":
        raise ValueError("invalid source-relative movie")
    return p


def safe_output(value):
    path=Path(value).resolve();local=(ROOT/".local").resolve()
    if local not in path.parents:
        raise ValueError("visual review must use a dedicated project .local directory")
    return path


def strict_movies(document):
    if document.get("requested_model")!="realesrgan-x4plus" or document.get("require_original_provenance") is not True:
        raise ValueError("visual review requires the strict original/x4plus progress report")
    result=[]
    for row in document.get("details",[]):
        if (row.get("status")=="verified" and row.get("output_validated") is True
                and row.get("recorded_model")=="realesrgan-x4plus" and row.get("origin")=="original-bink"
                and row.get("historical_source_and_weights_verified") is True):
            name=str(safe_movie(row["movie"]))
            if name in result:raise ValueError("duplicate verified movie")
            result.append(name)
    if not result:raise ValueError("no strict completed movies to sample")
    return result


def write_json(path,value):
    temporary=path.with_suffix(path.suffix+".tmp")
    temporary.write_text(json.dumps(value,indent=2,allow_nan=False)+"\n",encoding="utf-8")
    os.replace(temporary,path)


def packet_timeline(ff,path):
    command=[video.binary(ff,"ffprobe"),"-v","error","-threads","2","-select_streams","v:0",
             "-show_packets","-show_entries","stream=time_base,start_pts,duration_ts,width,height:packet=pts,duration",
             "-of","json",str(path)]
    completed=subprocess.run(command,capture_output=True,text=True,check=True)
    if completed.stderr.strip():raise ValueError("packet probe reported an error: "+completed.stderr[-500:])
    document=json.loads(completed.stdout);streams=document.get("streams",[])
    if len(streams)!=1:raise ValueError("expected exactly one selected video stream")
    stream=streams[0];base=Fraction(stream["time_base"]);times={};durations={}
    for packet in document.get("packets",[]):
        pts=int(packet["pts"]);time=pts*base
        if time in times:raise ValueError("duplicate movie presentation timestamp")
        times[time]=pts;durations[time]=int(packet["duration"])*base
    if not times or base<=0:raise ValueError("movie packet timeline is empty/invalid")
    return {"stream":stream,"base":base,"times":times,"durations":durations}


def matching_times(source,output):
    if set(source["times"])!=set(output["times"]) or source["durations"]!=output["durations"]:
        raise ValueError("source/output packet presentation times or durations differ")
    return sorted(source["times"])


def extract(ff,path,pts,base,destination):
    time=pts*base;seek=max(Fraction(0),time-1)
    command=[video.binary(ff,"ffmpeg"),"-hide_banner","-nostdin","-v","info","-threads","2",
             "-filter_threads","1","-filter_complex_threads","1","-copyts","-ss",f"{float(seek):.12f}",
             "-i",str(path),"-an","-sn","-vf",f"select=eq(pts\\,{pts}),showinfo",
             "-frames:v","1","-fps_mode","passthrough","-threads:v","1","-update","1","-y",str(destination)]
    completed=subprocess.run(command,capture_output=True,text=True,check=True)
    config=re.search(r"config in time_base:\s*(\d+/\d+)",completed.stderr)
    observed=re.findall(r"\bn:\s*\d+\s+pts:\s*(-?\d+)\s+pts_time:",completed.stderr)
    if not config or len(observed)!=1 or int(observed[0])*Fraction(config[1])!=time:
        raise ValueError(f"decoded frame PTS did not match requested source time for {path.name}")
    with Image.open(destination) as image:
        image.load();size=list(image.size)
    return {"requested_pts":pts,"requested_time_base":str(base),"time":str(time),
            "observed_pts":int(observed[0]),"observed_time_base":config[1],"size":size,
            "png_sha256":video.sha256(destination),"decoder_threads":2,"filter_threads":1,"png_threads":1}


def nonblack_stats(path):
    with Image.open(path) as image:rgb=np.asarray(image.convert("RGB"),dtype=np.float32)
    luma=rgb@np.asarray([.2126,.7152,.0722],np.float32)
    return {"mean_luma":float(luma.mean()),"std_luma":float(luma.std()),
            "fraction_luma_over_20":float(np.mean(luma>20))}


def meaningful(stats):
    return stats["mean_luma"]>=3 and stats["std_luma"]>=5 and stats["fraction_luma_over_20"]>=.005


def candidates(times,fraction,used):
    maximum=times[-1]
    targets=[max(Fraction(0),min(Fraction(1),fraction+offset))*maximum for offset in
             map(Fraction,("0",".05","-.05",".10","-.10",".15","-.15"))]
    result=[]
    for target in targets:
        nearest=min(times,key=lambda t:abs(t-target))
        if nearest not in used and nearest not in result:result.append(nearest)
    return result


def detail_box(image):
    rgb=np.asarray(image.convert("RGB"),dtype=np.float32);height,width=rgb.shape[:2]
    w,h=min(128,width),min(96,height);luma=rgb.mean(axis=2)
    gradient=np.abs(np.diff(luma,axis=0,append=luma[-1:]))+np.abs(np.diff(luma,axis=1,append=luma[:,-1:]))
    boxes=[]
    for cy in np.linspace(.28*height,.78*height,7):
        for cx in np.linspace(.25*width,.75*width,9):
            x=min(max(0,round(cx-w/2)),width-w);y=min(max(0,round(cy-h/2)),height-h)
            boxes.append((float(gradient[y:y+h,x:x+w].mean()),(x,y,x+w,y+h)))
    return max(boxes,key=lambda value:value[0])[1]


def sheets(directory,samples):
    overview_rows=[];detail_rows=[]
    for index,sample in enumerate(samples):
        with Image.open(directory/sample["source_png"]) as opened:source=opened.convert("RGB")
        with Image.open(directory/sample["output_png"]) as opened:output=opened.convert("RGB")
        if output.size!=(source.width*3,source.height*3):raise ValueError("sample dimensions are not source3x")
        width=min(640,source.width);height=round(source.height*width/source.width)
        overview=Image.new("RGB",(width*2,height+30),(24,24,24));draw=ImageDraw.Draw(overview)
        overview.paste(source.resize((width,height),Image.Resampling.LANCZOS),(0,30))
        overview.paste(output.resize((width,height),Image.Resampling.LANCZOS),(width,30))
        label=f"t={float(Fraction(sample['time'])):.6f}s"
        draw.text((5,7),"Original Bink / "+label,fill="white");draw.text((width+5,7),"Current x4plus / "+label,fill="white")
        overview_rows.append(overview)
        box=detail_box(source);sample["detail_source_box"]=list(box)
        native=tuple(value*3 for value in box)
        baseline=source.crop(box).resize(((box[2]-box[0])*3,(box[3]-box[1])*3),Image.Resampling.LANCZOS)
        enhanced=output.crop(native);w,h=baseline.size
        detail=Image.new("RGB",(w*2,h+30),(24,24,24));draw=ImageDraw.Draw(detail)
        detail.paste(baseline,(0,30));detail.paste(enhanced,(w,30))
        draw.text((5,7),"Original Lanczos display / "+label,fill="white")
        draw.text((w+5,7),"x4plus native 3x / "+label,fill="white")
        detail_rows.append(detail)
    result={}
    for name,rows in (("overview.png",overview_rows),("detail.png",detail_rows)):
        canvas=Image.new("RGB",(max(i.width for i in rows),sum(i.height for i in rows)),(24,24,24));y=0
        for row in rows:canvas.paste(row,(0,y));y+=row.height
        canvas.save(directory/name)
        result[name]={"sha256":video.sha256(directory/name),"size":list(canvas.size)}
    return result


def verify_inputs(name,identity):
    relative=safe_movie(name);source=video.ASSETS/relative;output=(video.OUT/relative).with_suffix(".mp4")
    sidecar=output.with_suffix(".upscale.json")
    saved=json.loads(sidecar.read_text(encoding="utf-8"));recipe=saved.get("recipe",{})
    hashes={"source_sha256":video.sha256(source),"output_sha256":video.sha256(output),"sidecar_sha256":video.sha256(sidecar)}
    if (saved.get("origin")!="original-bink" or saved.get("historical_source_and_weights_verified") is not True
            or recipe.get("model")!=identity or recipe.get("source_sha256")!=hashes["source_sha256"]
            or saved.get("output_sha256")!=hashes["output_sha256"] or recipe.get("output_scale")!=3):
        raise ValueError("strict source/model/output provenance does not match current files: "+name)
    return source,output,sidecar,saved,hashes


def sample_clip(ff,directory,name,identity):
    source,output,sidecar,saved,hashes=verify_inputs(name,identity)
    a,b=packet_timeline(ff,source),packet_timeline(ff,output);times=matching_times(a,b)
    if len(times)!=saved["recipe"]["source"]["frames"]:raise ValueError("source packet count differs from strict recipe")
    directory.mkdir(parents=True,exist_ok=True);samples=[];used=set()
    for number,fraction in enumerate(map(Fraction,(".2",".5",".8")),1):
        chosen=None
        for time in candidates(times,fraction,used):
            source_png=directory/f"frame{number}-source.png"
            source_record=extract(ff,source,a["times"][time],a["base"],source_png)
            stats=nonblack_stats(source_png)
            if meaningful(stats):chosen=(time,source_png,source_record,stats);break
        if chosen is None:raise ValueError(f"no meaningful nonblack candidate near phase {fraction}: {name}")
        time,source_png,source_record,stats=chosen;used.add(time)
        output_png=directory/f"frame{number}-output.png"
        output_record=extract(ff,output,b["times"][time],b["base"],output_png)
        samples.append({"time":str(time),"phase_target":str(fraction),"source_png":source_png.name,"output_png":output_png.name,
                        "source_decode":source_record,"output_decode":output_record,"nonblack":stats})
    sheet_records=sheets(directory,samples)
    if {"source_sha256":video.sha256(source),"output_sha256":video.sha256(output),"sidecar_sha256":video.sha256(sidecar)}!=hashes:
        raise ValueError("media/provenance changed during visual sampling")
    result={"movie":name,"phase":"sampled-awaiting-visual-review","source":str(source.resolve()),"output":str(output.resolve()),
            "sidecar":str(sidecar.resolve()),**hashes,"model":identity,"source_stream":a["stream"],"output_stream":b["stream"],
            "packet_timestamps_matched":len(times),"samples":samples,"sheets":sheet_records,
            "temporal_caveat":"Three exact matched-PTS stills cannot certify temporal consistency, flicker or every frame's quality."}
    write_json(directory/"samples.json",result)
    return result


def completed_record(directory,name,identity):
    path=directory/"samples.json"
    if not path.exists():return None
    saved=json.loads(path.read_text(encoding="utf-8"));_s,_o,_m,_record,hashes=verify_inputs(name,identity)
    if saved.get("movie")!=name or saved.get("model")!=identity or any(saved.get(k)!=v for k,v in hashes.items()):
        raise ValueError("existing visual samples belong to changed media/provenance")
    for sample in saved["samples"]:
        for prefix in ("source","output"):
            if video.sha256(directory/sample[prefix+"_png"])!=sample[prefix+"_decode"]["png_sha256"]:
                raise ValueError("existing visual sample PNG changed")
    for sheet,record in saved["sheets"].items():
        if video.sha256(directory/sheet)!=record["sha256"]:raise ValueError("existing comparison sheet changed")
    return saved


def run(progress,output_dir,only=None):
    output_dir=safe_output(output_dir);identity=video.model_identity("realesrgan-x4plus")
    manifest_path=output_dir/"manifest.json";snapshot_path=output_dir/"progress-snapshot.json"
    if output_dir.exists():
        if not manifest_path.is_file() or not snapshot_path.is_file():raise ValueError("existing review directory is not a resumable sampler run")
        manifest=json.loads(manifest_path.read_text(encoding="utf-8"));raw=snapshot_path.read_bytes()
        if (manifest.get("format")!=FORMAT or manifest.get("snapshot_sha256")!=hashlib.sha256(raw).hexdigest()
                or manifest.get("model")!=identity or manifest.get("sampler_sha256")!=video.sha256(Path(__file__))):
            raise ValueError("existing visual review snapshot/model/sampler differs")
    else:
        raw=Path(progress).read_bytes();selected=strict_movies(json.loads(raw.decode("utf-8")))
        output_dir.mkdir(parents=True)
        snapshot_path.write_bytes(raw)
        manifest={"format":FORMAT,"snapshot_sha256":hashlib.sha256(raw).hexdigest(),"sampler_sha256":video.sha256(Path(__file__)),
                  "model":identity,"selected_movies":selected,"completed":{},"failures":{},"gpu_used":False,
                  "media_modified":False,"sampling_only":True}
        write_json(manifest_path,manifest)
    selected=strict_movies(json.loads(raw.decode("utf-8")))
    if manifest["selected_movies"]!=selected:raise ValueError("saved verified selection differs from snapshot")
    requested=set(only or selected)
    if requested-set(selected):raise ValueError("requested movie was not strict verified in initial snapshot")
    ff=video.ffmpeg_bin()
    for name in selected:
        if name not in requested:continue
        directory=output_dir/str(safe_movie(name).with_suffix("")).replace("/","__")
        try:
            result=completed_record(directory,name,identity) or sample_clip(ff,directory,name,identity)
            manifest["completed"][name]={"directory":directory.name,"samples_sha256":video.sha256(directory/"samples.json"),"sample_count":len(result["samples"])}
            manifest["failures"].pop(name,None)
            write_json(manifest_path,manifest)
            print(f"Visual samples {len(manifest['completed'])}/{len(selected)}: {name}",flush=True)
        except (OSError,ValueError,KeyError,subprocess.SubprocessError) as error:
            manifest["failures"][name]=str(error);write_json(manifest_path,manifest)
            print(f"Visual sampling failed {name}: {error}",flush=True)
    return 1 if manifest["failures"] else 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--progress",type=Path,default=ROOT/".local/overnight_pipeline/video-progress.json")
    parser.add_argument("--output-dir",type=Path,default=ROOT/".local/video-visual-review-20261025")
    parser.add_argument("--only",help="comma-separated strict verified source-relative .bik names")
    args=parser.parse_args()
    return run(args.progress,args.output_dir,args.only.split(",") if args.only else None)


if __name__=="__main__":raise SystemExit(main())
