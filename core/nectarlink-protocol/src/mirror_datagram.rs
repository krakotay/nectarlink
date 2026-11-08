// SPDX-License-Identifier: MPL-2.0
//! Bounded H.264 I/P fragments with explicit reference-chain recovery.

use std::{
    collections::BTreeMap,
    time::{Duration, Instant},
};

pub const MAGIC: &[u8; 4] = b"NLMV";
pub const HEADER: usize = 48;
pub const MAX_FRAME_BYTES: usize = 2 << 20;
pub const MAX_DATAGRAM_BYTES: usize = 1200;
pub const FRAME_LIFETIME: Duration = Duration::from_millis(35);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Header {
    pub keyframe: bool,
    pub stream: u64,
    pub generation: u32,
    pub frame: u64,
    pub time_us: u64,
    pub sent_ms: u32,
    pub length: usize,
    pub chunk: usize,
    pub index: usize,
    pub count: usize,
}

impl Header {
    pub fn parse(bytes: &[u8]) -> Option<(Self, &[u8])> {
        if bytes.len() <= HEADER
            || bytes.len() > MAX_DATAGRAM_BYTES
            || &bytes[..4] != MAGIC
            || bytes[4] != 1
            || bytes[5] > 1
        {
            return None;
        }
        let u16_at = |i| u16::from_be_bytes(bytes[i..i + 2].try_into().unwrap()) as usize;
        let u32_at = |i| u32::from_be_bytes(bytes[i..i + 4].try_into().unwrap());
        let u64_at = |i| u64::from_be_bytes(bytes[i..i + 8].try_into().unwrap());
        let h = Self {
            keyframe: bytes[5] == 1,
            index: u16_at(6),
            count: u16_at(8),
            chunk: u16_at(10),
            stream: u64_at(12),
            generation: u32_at(20),
            frame: u64_at(24),
            time_us: u64_at(32),
            length: u32_at(40) as usize,
            sent_ms: u32_at(44),
        };
        if h.stream == 0
            || h.generation == 0
            || h.frame == 0
            || h.length == 0
            || h.length > MAX_FRAME_BYTES
            || h.chunk == 0
            || h.chunk > MAX_DATAGRAM_BYTES - HEADER
            || h.count != h.length.div_ceil(h.chunk)
            || h.index >= h.count
        {
            return None;
        }
        let payload = &bytes[HEADER..];
        if payload.len() != h.chunk.min(h.length - h.index * h.chunk) {
            return None;
        }
        Some((h, payload))
    }
}

/// Fragments one complete independent access unit. Empty means it cannot fit.
pub fn fragment(mut h: Header, data: &[u8], mtu: usize) -> Vec<Vec<u8>> {
    let mtu = mtu.min(MAX_DATAGRAM_BYTES);
    if mtu <= HEADER || data.is_empty() || data.len() > MAX_FRAME_BYTES {
        return Vec::new();
    }
    h.chunk = mtu - HEADER;
    h.length = data.len();
    h.count = data.len().div_ceil(h.chunk);
    if h.count > u16::MAX as usize {
        return Vec::new();
    }
    data.chunks(h.chunk)
        .enumerate()
        .map(|(index, payload)| {
            let mut out = Vec::with_capacity(HEADER + payload.len());
            out.extend_from_slice(MAGIC);
            out.extend_from_slice(&[1, u8::from(h.keyframe)]); // version, IDR flag
            out.extend_from_slice(&(index as u16).to_be_bytes());
            out.extend_from_slice(&(h.count as u16).to_be_bytes());
            out.extend_from_slice(&(h.chunk as u16).to_be_bytes());
            out.extend_from_slice(&h.stream.to_be_bytes());
            out.extend_from_slice(&h.generation.to_be_bytes());
            out.extend_from_slice(&h.frame.to_be_bytes());
            out.extend_from_slice(&h.time_us.to_be_bytes());
            out.extend_from_slice(&(h.length as u32).to_be_bytes());
            out.extend_from_slice(&h.sent_ms.to_be_bytes());
            out.extend_from_slice(payload);
            out
        })
        .collect()
}

#[derive(Debug)]
struct Partial {
    header: Header,
    since: Instant,
    data: Vec<u8>,
    seen: Vec<bool>,
    remaining: usize,
}

/// Four access units at most. Reordering gets 5 ms, incomplete frames 35 ms.
/// A confirmed loss abandons dependent P pictures until a complete IDR arrives.
#[derive(Debug)]
pub struct Reassembler {
    stream: u64,
    generation: u32,
    since: Instant,
    best_transit_ms: Option<i128>,
    floor: u64,
    next: Option<u64>,
    gap_since: Option<Instant>,
    partial: BTreeMap<u64, Partial>,
}

pub type Completed = (bool, u64, Vec<u8>);
impl Reassembler {
    pub fn new(stream: u64, generation: u32, now: Instant) -> Self {
        Self {
            stream,
            generation,
            since: now,
            best_transit_ms: None,
            floor: 0,
            next: None,
            gap_since: None,
            partial: BTreeMap::new(),
        }
    }
    pub fn needs_keyframe(&self) -> bool {
        self.next.is_none()
    }
    fn abandon(&mut self, floor: u64) {
        self.floor = self.floor.max(floor);
        self.partial.clear();
        self.next = None;
        self.gap_since = None;
    }
    pub fn push(&mut self, bytes: &[u8], now: Instant) -> Vec<Completed> {
        let Some((h, payload)) = Header::parse(bytes) else { return vec![] };
        if h.stream != self.stream || h.generation != self.generation || h.frame <= self.floor {
            return vec![];
        }
        let transit = now.saturating_duration_since(self.since).as_millis() as i128 - i128::from(h.sent_ms);
        let best = self.best_transit_ms.map_or(transit, |best| best.min(transit));
        self.best_transit_ms = Some(best);
        if transit - best >= FRAME_LIFETIME.as_millis() as i128 {
            self.abandon(h.frame);
            return vec![];
        }
        if self.needs_keyframe() && !h.keyframe && !self.partial.values().any(|p| p.header.keyframe) {
            return vec![];
        }
        if !self.partial.contains_key(&h.frame) && self.partial.len() >= 4 {
            let floor = *self.partial.last_key_value().unwrap().0;
            self.abandon(floor);
            if !h.keyframe || h.frame <= floor {
                return vec![];
            }
        }
        let p = self.partial.entry(h.frame).or_insert_with(|| Partial {
            header: h,
            since: now,
            data: vec![0; h.length],
            seen: vec![false; h.count],
            remaining: h.count,
        });
        let mut expected = p.header;
        expected.index = h.index;
        if expected != h {
            return vec![];
        }
        if !p.seen[h.index] {
            let start = h.index * h.chunk;
            p.data[start..start + payload.len()].copy_from_slice(payload);
            p.seen[h.index] = true;
            p.remaining -= 1;
        }
        self.poll(now)
    }
    pub fn poll(&mut self, now: Instant) -> Vec<Completed> {
        let mut out = Vec::new();
        // An IDR is a safe new root even if an older picture is incomplete.
        if let Some(id) = self
            .partial
            .iter()
            .rev()
            .find_map(|(&id, p)| (p.header.keyframe && p.remaining == 0).then_some(id))
        {
            let p = self.partial.remove(&id).unwrap();
            self.partial.retain(|&n, _| n > id);
            self.floor = id;
            self.next = id.checked_add(1);
            self.gap_since = None;
            out.push((true, p.header.time_us, p.data));
        }
        while let Some(next) = self.next {
            if !self.partial.get(&next).is_some_and(|p| p.remaining == 0) {
                break;
            }
            let p = self.partial.remove(&next).unwrap();
            self.floor = next;
            self.next = next.checked_add(1);
            self.gap_since = None;
            out.push((p.header.keyframe, p.header.time_us, p.data));
        }
        let expired = self.partial.values().any(|p| now.saturating_duration_since(p.since) >= FRAME_LIFETIME);
        let gap =
            self.next.is_some_and(|next| self.partial.first_key_value().is_some_and(|(&id, _)| id > next));
        if gap {
            self.gap_since.get_or_insert(now);
        } else {
            self.gap_since = None;
        }
        let lost = self
            .gap_since
            .is_some_and(|since| now.saturating_duration_since(since) >= Duration::from_millis(5));
        if expired || lost {
            let floor = self.partial.last_key_value().map_or(self.floor, |(&id, _)| id);
            self.abandon(floor);
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn packets(id: u64, keyframe: bool, size: usize) -> Vec<Vec<u8>> {
        fragment(
            Header {
                keyframe,
                stream: 7,
                generation: 1,
                frame: id,
                time_us: id * 10,
                sent_ms: 0,
                length: 0,
                chunk: 0,
                index: 0,
                count: 0,
            },
            &vec![id as u8; size],
            1200,
        )
    }
    #[test]
    fn reorder_duplicates_and_dependencies() {
        let t = Instant::now();
        let mut r = Reassembler::new(7, 1, t);
        assert_eq!(r.push(&packets(1, true, 1)[0], t).len(), 1);
        let p = packets(2, false, 2500);
        assert!(r.push(&p[2], t).is_empty());
        assert!(r.push(&p[2], t).is_empty());
        assert!(r.push(&packets(3, false, 1)[0], t).is_empty());
        assert!(r.push(&p[0], t).is_empty());
        let ready = r.push(&p[1], t + Duration::from_millis(1));
        assert_eq!(ready.iter().map(|p| p.1).collect::<Vec<_>>(), vec![20, 30]);
        assert!(!r.needs_keyframe());
    }
    #[test]
    fn loss_stops_pictures_until_new_idr() {
        let t = Instant::now();
        let mut r = Reassembler::new(7, 1, t);
        r.push(&packets(1, true, 1)[0], t);
        assert!(r.push(&packets(3, false, 1)[0], t).is_empty());
        assert!(r.poll(t + Duration::from_millis(5)).is_empty());
        assert!(r.needs_keyframe());
        assert!(r.push(&packets(2, false, 1)[0], t).is_empty());
        assert!(r.push(&packets(4, false, 1)[0], t).is_empty());
        assert_eq!(r.push(&packets(5, true, 1)[0], t).len(), 1);
        assert_eq!(r.push(&packets(6, false, 1)[0], t).len(), 1);
    }
    #[test]
    fn expired_idr_cannot_revive_and_generations_do_not_mix() {
        let t = Instant::now();
        let mut r = Reassembler::new(7, 1, t);
        let p = packets(1, true, 2000);
        r.push(&p[0], t);
        r.poll(t + FRAME_LIFETIME);
        assert!(r.push(&p[1], t + FRAME_LIFETIME).is_empty());
        assert!(r.push(&p[0], t + FRAME_LIFETIME).is_empty());
        assert!(Reassembler::new(7, 2, t).push(&packets(2, true, 1)[0], t).is_empty());
    }
    #[test]
    fn newer_idr_supersedes_incomplete_reference() {
        let t = Instant::now();
        let mut r = Reassembler::new(7, 1, t);
        r.push(&packets(1, true, 2000)[0], t);
        assert_eq!(r.push(&packets(4, true, 1)[0], t).len(), 1);
        assert!(r.push(&packets(1, true, 2000)[1], t).is_empty());
    }
    #[test]
    fn malformed_fragments_rejected_before_allocation() {
        let good = packets(1, true, 2000).remove(0);
        for i in [4, 5, 6, 8, 10, 40] {
            let mut bad = good.clone();
            bad[i] = 255;
            assert!(Header::parse(&bad).is_none());
        }
        assert!(Header::parse(&good[..20]).is_none());
        assert!(fragment(Header::parse(&good).unwrap().0, &vec![0; MAX_FRAME_BYTES + 1], 1200).is_empty());
    }
}
