// SPDX-License-Identifier: MPL-2.0
//! Bounded I/P datagram transport; the reliable stream only carries setup.

use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    time::{Duration, Instant},
};

use bytes::Bytes;
use iroh::endpoint::{Connection, RecvStream, SendStream, VarInt};
use nectarlink_protocol::{
    PacketKind,
    messages::{MirrorConfig, MirrorDatagramConfig, mirror},
    mirror_datagram::{self as wire, Header, Reassembler},
    read_video_packet, video_packet_header,
};

use super::{MirrorQueue, MirrorSend, MirrorSink, MirrorStream};
use crate::{NodeEvent, node::Shared, session::Session};

const SEND_LIFETIME: Duration = Duration::from_millis(35);
const MAX_STREAMS: usize = 16;

#[derive(Debug)]
struct Picture {
    time_us: u64,
    sequence: u64,
    keyframe: bool,
    revision: u64,
    data: Vec<u8>,
    queued: Instant,
}

#[derive(Debug)]
struct Pending {
    config: Option<Vec<u8>>,
    picture: Option<Picture>,
    sequence: u64,
    revision: u64,
    needs_keyframe: bool,
}
impl Default for Pending {
    fn default() -> Self {
        Self { config: None, picture: None, sequence: 0, revision: 0, needs_keyframe: true }
    }
}
#[derive(Debug, Default)]
pub(super) struct Latest {
    pending: Mutex<Pending>,
    ready: tokio::sync::Notify,
}
impl Latest {
    pub(super) fn send(&self, kind: PacketKind, time_us: u64, data: Vec<u8>) -> MirrorSend {
        let mut p = self.pending.lock().unwrap_or_else(|e| e.into_inner());
        match kind {
            PacketKind::Config => {
                if !MirrorConfig::from_cbor(&data).is_ok_and(|c| c.is_valid() && c.codec == mirror::H264) {
                    return MirrorSend::Closed;
                }
                p.config = Some(data);
                p.picture = None;
                p.needs_keyframe = true;
                p.revision += 1;
            }
            PacketKind::Keyframe | PacketKind::Frame => {
                // Allocate before dropping: a skipped producer frame must remain visible as a sequence gap.
                p.sequence += 1;
                let keyframe = kind == PacketKind::Keyframe;
                if data.is_empty() || data.len() > wire::MAX_FRAME_BYTES {
                    p.picture = None;
                    p.needs_keyframe = true;
                    p.revision += 1;
                    return MirrorSend::NeedKeyframe;
                }
                if !keyframe && (p.needs_keyframe || p.picture.is_some()) {
                    // An unconsumed IDR may replace this entire P chain safely.
                    if !p.picture.as_ref().is_some_and(|f| f.keyframe) {
                        p.picture = None;
                        p.needs_keyframe = true;
                        p.revision += 1;
                    } else {
                        p.needs_keyframe = true;
                    }
                    return MirrorSend::NeedKeyframe;
                }
                if keyframe {
                    p.revision += 1;
                    p.needs_keyframe = false;
                }
                p.picture = Some(Picture {
                    time_us,
                    sequence: p.sequence,
                    keyframe,
                    revision: p.revision,
                    data,
                    queued: Instant::now(),
                });
            }
        }
        drop(p);
        self.ready.notify_one();
        MirrorSend::Queued
    }
    fn take(&self) -> (Option<Vec<u8>>, Option<Picture>) {
        let mut p = self.pending.lock().unwrap_or_else(|e| e.into_inner());
        (p.config.take(), p.picture.take())
    }
    fn superseded(&self, picture: &Picture) -> bool {
        self.pending.lock().unwrap_or_else(|e| e.into_inner()).revision != picture.revision
    }
    fn abandon(&self, picture: &Picture) {
        let mut p = self.pending.lock().unwrap_or_else(|e| e.into_inner());
        if p.revision == picture.revision {
            p.picture = None;
            p.needs_keyframe = true;
            p.revision += 1;
        }
    }
}

impl MirrorStream {
    pub(super) fn spawn_datagram(
        conn: Connection,
        mut send: SendStream,
        mut recv: RecvStream,
        on_end: Option<(Arc<Shared>, i64, Instant)>,
    ) -> Self {
        let latest = Arc::new(Latest::default());
        let queue = latest.clone();
        let closed = Arc::new(AtomicBool::new(false));
        let done = closed.clone();
        let stop = Arc::new(tokio::sync::Notify::new());
        let stopped = stop.clone();
        let _ = send.set_priority(super::VIDEO_PRIORITY);
        tokio::spawn(async move {
            let stream = rand::random::<u64>().max(1);
            let since = Instant::now();
            let mut generation = 0u32;
            let mut response = [0u8; 1];
            let mut sent = 0u64;
            let mut dropped = 0u64;
            'stream: loop {
                tokio::select! {
                    _ = stopped.notified() => break,
                    read = recv.read(&mut response) => {
                        if !matches!(read, Ok(Some(_))) { break; }
                        continue;
                    }
                    () = latest.ready.notified() => {}
                }
                if done.load(Ordering::Acquire) {
                    break;
                }
                let (config, picture) = latest.take();
                if let Some(config) = config {
                    let Ok(video) = MirrorConfig::from_cbor(&config) else { break };
                    let Some(next) = generation.checked_add(1) else { break };
                    generation = next;
                    let config = MirrorDatagramConfig { video, stream, generation };
                    let data = config.to_cbor();
                    let Ok(header) = video_packet_header(PacketKind::Config, 0, data.len()) else { break };
                    let setup = async {
                        send.write_all(&header).await.map_err(crate::error::net)?;
                        send.write_all(&data).await.map_err(crate::error::net)?;
                        recv.read_exact(&mut response).await.map_err(crate::error::net)?;
                        Ok::<(), crate::Error>(())
                    };
                    let configured = tokio::select! {
                        _ = stopped.notified() => false,
                        result = tokio::time::timeout(Duration::from_secs(3), setup) => match result {
                            Ok(Ok(())) => response[0] == 1,
                            Ok(Err(error)) => { tracing::error!(%error,"Mirror DATAGRAM setup failed"); false },
                            Err(error) => { tracing::error!(%error,"Mirror DATAGRAM setup timed out"); false },
                        },
                    };
                    if !configured {
                        break;
                    }
                }
                let Some(picture) = picture else { continue };
                if generation == 0 || picture.queued.elapsed() >= SEND_LIFETIME || latest.superseded(&picture)
                {
                    latest.abandon(&picture);
                    dropped += 1;
                    continue;
                }
                let Some(mtu) = conn.max_datagram_size() else { break };
                let header = Header {
                    stream,
                    generation,
                    keyframe: picture.keyframe,
                    frame: picture.sequence,
                    time_us: picture.time_us,
                    sent_ms: since.elapsed().as_millis() as u32,
                    length: 0,
                    chunk: 0,
                    index: 0,
                    count: 0,
                };
                let parts: Vec<Bytes> =
                    wire::fragment(header, &picture.data, mtu).into_iter().map(Bytes::from).collect();
                if parts.is_empty() {
                    latest.abandon(&picture);
                    dropped += 1;
                    continue;
                }
                let mut offset = 0;
                while offset < parts.len() {
                    if done.load(Ordering::Acquire) {
                        break 'stream;
                    }
                    if picture.queued.elapsed() >= SEND_LIFETIME || latest.superseded(&picture) {
                        latest.abandon(&picture);
                        dropped += 1;
                        break;
                    }
                    let space = conn.datagram_send_buffer_space();
                    let mut bytes = 0;
                    let count = parts[offset..]
                        .iter()
                        .take(32)
                        .take_while(|p| {
                            bytes += p.len();
                            bytes <= space
                        })
                        .count();
                    if count > 0 {
                        if let Err(error) = conn.send_many_datagrams(&parts[offset..offset + count]) {
                            tracing::error!(%error, "Mirror DATAGRAM send failed");
                            latest.abandon(&picture);
                            dropped += 1;
                            break;
                        }
                        offset += count;
                        tokio::task::yield_now().await;
                    } else {
                        tokio::select! {
                            _ = stopped.notified() => break 'stream,
                            () = tokio::time::sleep(Duration::from_millis(1)) => {},
                        }
                    }
                }
                if offset == parts.len() {
                    sent += 1;
                }
            }
            done.store(true, Ordering::Release);
            let _ = send.finish();
            tracing::info!(sent, dropped, "mirror datagram sender ended");
            if let Some((shared, row, started)) = on_end {
                shared.finish_timeline_session(row, started.elapsed().as_secs().max(1));
            }
        });
        Self {
            queue: MirrorQueue::Datagram(queue),
            closed,
            stop,
            resyncing: Mutex::new(false),
            audio: false,
            link_rate: Arc::new(AtomicU64::new(0)),
        }
    }
}

pub(crate) struct Incoming {
    sink: Arc<dyn MirrorSink>,
    frames: Reassembler,
    mirroring: u32,
    asked: Option<Instant>,
}

pub(crate) fn handle(session: &Session, bytes: &[u8]) {
    let Some((h, _)) = Header::parse(bytes) else { return };
    let mut incoming = session.mirror_datagrams.lock().unwrap_or_else(|e| e.into_inner());
    let Some(receiver) = incoming.get_mut(&h.stream) else { return };
    for (keyframe, time, data) in receiver.frames.push(bytes, Instant::now()) {
        receiver.sink.packet(keyframe, time, data);
    }
}

pub(crate) async fn receive(
    shared: Arc<Shared>,
    session: Arc<Session>,
    mut send: SendStream,
    mut recv: RecvStream,
) {
    let peer = session.peer;
    let allowed = shared.we_offer(mirror::VIEW)
        && shared.we_offer(mirror::DATAGRAM)
        && if super::is_local_desktop(&shared) {
            shared.toggle_on(&peer, super::TOGGLE)
        } else {
            shared.store.toggle(&peer, super::PC_MIRROR_TOGGLE).ok().flatten().unwrap_or(true)
        };
    let mut stream_id = None;
    let mut mirroring = None;
    let mut sink: Option<Arc<dyn MirrorSink>> = None;
    let mut stopped = Arc::new(tokio::sync::Notify::new());
    let started = Instant::now();
    let mut row = None;
    let mut timer = tokio::time::interval(Duration::from_millis(5));
    if allowed {
        'receive: loop {
            // Keep the partially-read reliable setup alive across expiry ticks.
            let read = read_video_packet(&mut recv);
            tokio::pin!(read);
            let packet = loop {
                tokio::select! {
                    _ = stopped.notified() => break 'receive,
                    p = &mut read => break p,
                    _ = timer.tick() => {
                        let request = {
                            let mut incoming = session.mirror_datagrams.lock().unwrap_or_else(|e| e.into_inner());
                            stream_id.and_then(|id| incoming.get_mut(&id)).and_then(|r: &mut Incoming| {
                                let now = Instant::now();
                                for (key,time,data) in r.frames.poll(now) { r.sink.packet(key,time,data); }
                                if r.frames.needs_keyframe() && r.asked.is_none_or(|t| now.duration_since(t) >= Duration::from_millis(100)) {
                                    r.asked = Some(now); Some(r.mirroring)
                                } else { None }
                            })
                        };
                        if let Some(id) = request { super::request_keyframe(&session,id).await; }
                        continue;
                    }
                }
            };
            let Ok(Some(packet)) = packet else { break };
            if packet.kind != PacketKind::Config {
                break;
            }
            let Ok(config) = MirrorDatagramConfig::from_cbor(&packet.data) else {
                break;
            };
            if !config.video.is_valid()
                || config.video.codec != mirror::H264
                || config.stream == 0
                || config.generation == 0
                || stream_id.is_some_and(|id| id != config.stream)
                || mirroring.is_some_and(|id| id != config.video.session)
            {
                break;
            }
            if sink.is_none() {
                sink = shared.platform.mirror_sink(&peer, config.video.session);
                if sink.is_none() {
                    break;
                }
                stopped = shared.new_mirror_stop(&peer, config.video.session, mirror::OP_VIDEO);
                mirroring = Some(config.video.session);
                row = shared.record_timeline(crate::timeline::NewTimelineEntry {
                    kind: crate::TimelineKind::Session,
                    device_id: peer,
                    device_name: shared.peer_name(&peer),
                    incoming: true,
                    timestamp: crate::now_unix(),
                    title: "Screen mirroring".into(),
                    detail: format!("{}×{} (datagram)", config.video.width, config.video.height),
                    target: "mirror".into(),
                    size_bytes: 0,
                    duration_secs: 0,
                    ref_id: None,
                });
                shared.emit(NodeEvent::Mirroring { device: peer, session: config.video.session, on: true });
            }
            let sink = sink.as_ref().unwrap();
            {
                let mut incoming = session.mirror_datagrams.lock().unwrap_or_else(|e| e.into_inner());
                if stream_id.is_none()
                    && (incoming.len() >= MAX_STREAMS || incoming.contains_key(&config.stream))
                {
                    break;
                }
                let mirroring = config.video.session;
                sink.config(config.video);
                incoming.insert(
                    config.stream,
                    Incoming {
                        sink: sink.clone(),
                        mirroring,
                        asked: None,
                        frames: Reassembler::new(config.stream, config.generation, Instant::now()),
                    },
                );
                stream_id = Some(config.stream);
            }
            // The sender waits for setup only; pictures have no acknowledgements.
            if send.write_all(&[1]).await.is_err() {
                break;
            }
        }
    }
    if let Some(id) = stream_id {
        session.mirror_datagrams.lock().unwrap_or_else(|e| e.into_inner()).remove(&id);
    }
    let _ = recv.stop(VarInt::from_u32(mirror::STOPPED));
    let _ = send.finish();
    if let Some(sink) = sink {
        sink.ended();
    }
    if let Some(row) = row {
        shared.finish_timeline_session(row, started.elapsed().as_secs().max(1));
    }
    if let Some(id) = mirroring {
        shared.emit(NodeEvent::Mirroring { device: peer, session: id, on: false });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn p_overflow_abandons_chain_and_preserves_sequence_gaps() {
        let q = Latest::default();
        assert_eq!(q.send(PacketKind::Keyframe, 1, vec![1]), MirrorSend::Queued);
        let (_, key) = q.take();
        let key = key.unwrap();
        assert_eq!(key.sequence, 1);
        assert_eq!(q.send(PacketKind::Frame, 2, vec![2]), MirrorSend::Queued);
        assert_eq!(q.send(PacketKind::Frame, 3, vec![3]), MirrorSend::NeedKeyframe);
        assert!(q.take().1.is_none());
        assert!(q.superseded(&key));
        assert_eq!(q.send(PacketKind::Frame, 4, vec![4]), MirrorSend::NeedKeyframe);
        assert_eq!(q.send(PacketKind::Keyframe, 5, vec![5]), MirrorSend::Queued);
        assert_eq!(q.take().1.unwrap().sequence, 5);
    }
    #[test]
    fn in_flight_picture_survives_one_queued_dependent_picture() {
        let q = Latest::default();
        q.send(PacketKind::Keyframe, 1, vec![1]);
        let (_, key) = q.take();
        q.send(PacketKind::Frame, 2, vec![2]);
        assert!(!q.superseded(&key.unwrap()));
        let (_, p) = q.take();
        let p = p.unwrap();
        q.send(PacketKind::Keyframe, 3, vec![3]);
        q.abandon(&p);
        assert!(q.take().1.unwrap().keyframe);
    }
}
