//! Optional exact uploaded-palette content witness. No hashes or approximate
//! float equality: every uploaded matrix bit and slot order participates.
//! Oversize/allocation refusal loses the baseline and conservatively mutates.
use crate::ffi::WgrMat4;

pub(super) struct PaletteContentWitness {
    bytes: Vec<u8>,
    stride: Option<u64>,
    image_serial: u64,
    known: bool,
}
impl Default for PaletteContentWitness {
    fn default() -> Self {
        Self { bytes: Vec::new(), stride: None, image_serial: 1, known: false }
    }
}
impl PaletteContentWitness {
    const MAX_BYTES: usize = 1024 * 1024;

    // A missing/refused snapshot cannot certify a cached image. Zero is sticky
    // after serial overflow, so no earlier publication can alias a new stamp.
    pub fn image_serial(&self) -> Option<u64> {
        (self.known && self.image_serial != 0).then_some(self.image_serial)
    }

    fn changed(&mut self) {
        if self.image_serial != 0 {
            self.image_serial = self.image_serial.checked_add(1).unwrap_or(0);
        }
    }

    fn refuse(&mut self) -> bool {
        self.bytes = Vec::new();
        self.stride = None;
        self.known = false;
        self.changed();
        true
    }

    // Called after accepted writes, including an empty current slot set. The
    // caller passes only complete blocks actually written, not a trailing tail.
    pub fn observe(&mut self, matrices: &[WgrMat4], stride: u64, replaced: bool) -> bool {
        let bytes: &[u8] = bytemuck::cast_slice(matrices);
        if bytes.len() > Self::MAX_BYTES || stride == 0 || self.image_serial == 0 {
            return self.refuse();
        }
        if self.known && !replaced && self.stride == Some(stride) && self.bytes == bytes {
            return false;
        }
        if bytes.len() > self.bytes.capacity() &&
            self.bytes.try_reserve_exact(bytes.len() - self.bytes.len()).is_err() {
            return self.refuse();
        }
        if self.bytes.capacity() > Self::MAX_BYTES {
            return self.refuse();
        }
        self.bytes.clear(); self.bytes.extend_from_slice(bytes);
        self.stride = Some(stride);
        self.known = true;
        self.changed();
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn palette_content_witness_exact_reuploads_preserve_but_pose_and_order_change() {
        let mut witness = PaletteContentWitness::default();
        let mut matrices = vec![[0.0; 16]; 256];
        matrices[128][4] = 1.0;
        assert_eq!(witness.image_serial(), None);
        assert!(witness.observe(&matrices, 8192, false));
        let first = witness.image_serial().unwrap();
        assert!(!witness.observe(&matrices, 8192, false));
        assert_eq!(witness.image_serial(), Some(first));
        matrices[0][3] = -0.0;
        assert!(witness.observe(&matrices, 8192, false), "signed zero bits participate");
        assert_eq!(witness.image_serial(), Some(first + 1));
        assert!(!witness.observe(&matrices, 8192, false));
        matrices.swap(0, 128);
        assert!(witness.observe(&matrices, 8192, false), "foreign slot reorder participates");
        assert!(witness.observe(&matrices[..128], 8192, false), "slot removal participates");
        assert!(witness.observe(&matrices[..128], 8192, true), "buffer replacement is conservative");
        assert!(witness.observe(&matrices[..128], 16384, false), "stride identity participates");
        assert!(witness.observe(&[], 16384, false));
        assert!(!witness.observe(&[], 16384, false));
    }
    #[test]
    fn palette_content_witness_capacity_refusal_forgets_every_previous_stamp() {
        let mut witness = PaletteContentWitness::default();
        let small = [[1.0; 16]; 128];
        assert!(witness.observe(&small, 8192, false));
        let before_refusal = witness.image_serial().unwrap();
        let too_large = vec![[0.0; 16]; PaletteContentWitness::MAX_BYTES / 64 + 1];
        assert!(witness.observe(&too_large, 8192, false));
        assert_eq!(witness.bytes.capacity(), 0);
        assert_eq!(witness.image_serial(), None);
        assert_eq!(witness.image_serial, before_refusal + 1);
        assert!(witness.observe(&too_large, 8192, false));
        assert!(witness.observe(&small, 8192, false));
        assert!(witness.image_serial().unwrap() > before_refusal);
        assert!(!witness.observe(&small, 8192, false));
        assert!(witness.observe(&small, 0, false));
        assert_eq!(witness.stride, None);
        assert_eq!(witness.image_serial(), None);
        witness.image_serial = u64::MAX;
        assert!(witness.observe(&small, 8192, false));
        assert_eq!(witness.image_serial, 0);
        assert_eq!(witness.image_serial(), None);
    }
}
