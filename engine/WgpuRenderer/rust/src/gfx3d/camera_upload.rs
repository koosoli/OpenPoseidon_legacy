//! Coalesce the existing camera-uniform writes without changing their offsets or values.
use std::sync::OnceLock;

pub(super) struct CameraUpload<'a> {
    queue: &'a wgpu::Queue,
    buffer: &'a wgpu::Buffer,
    bytes: Option<Vec<u8>>,
}

fn copy_write(bytes: &mut [u8], offset: u64, value: &[u8]) {
    let start = usize::try_from(offset).expect("camera offset fits usize");
    let end = start.checked_add(value.len()).expect("camera write overflow");
    bytes[start..end].copy_from_slice(value);
}

impl<'a> CameraUpload<'a> {
    pub(super) fn new(queue: &'a wgpu::Queue, buffer: &'a wgpu::Buffer, size: u64) -> Self {
        static ENABLED: OnceLock<bool> = OnceLock::new();
        let enabled = *ENABLED.get_or_init(|| {
            std::env::var("WGR_CAMERA_UPLOAD_BATCH").map_or(true, |v| v != "0")
        });
        Self {
            queue,
            buffer,
            bytes: enabled.then(|| vec![0; usize::try_from(size).expect("camera size fits usize")]),
        }
    }

    pub(super) fn write_buffer(&mut self, buffer: &wgpu::Buffer, offset: u64, value: &[u8]) {
        if let Some(bytes) = &mut self.bytes {
            copy_write(bytes, offset, value);
        } else {
            self.queue.write_buffer(buffer, offset, value);
        }
    }

    pub(super) fn finish(self) {
        if let Some(bytes) = self.bytes {
            self.queue.write_buffer(self.buffer, 0, &bytes);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::copy_write;

    #[test]
    fn camera_upload_preserves_offsets_overwrites_and_camera_padding() {
        // Several aligned camera slots; only the binding range is visible to shaders.
        let stride = 256;
        let binding = 208;
        let size = stride * 3 + binding;
        let mut batched = vec![0; size];
        let mut reference = vec![0xcd; size];
        for camera in 0..4 {
            for lane in 0..13 {
                let offset = camera * stride + lane * 16;
                let values = [camera as u32, lane as u32, f32::NAN.to_bits(), 0x80000000];
                let bytes = bytemuck::cast_slice(&values);
                copy_write(&mut batched, offset as u64, bytes);
                reference[offset..offset + 16].copy_from_slice(bytes);
            }
            let offset = camera * stride + 16;
            copy_write(&mut batched, offset as u64, &[0; 16]);
            reference[offset..offset + 16].fill(0);
            assert_eq!(batched[camera * stride..camera * stride + binding],
                       reference[camera * stride..camera * stride + binding]);
        }
        assert!(batched[binding..stride].iter().all(|b| *b == 0));
    }

    #[test]
    #[should_panic]
    fn camera_upload_rejects_crossing_allocation() {
        copy_write(&mut [0; 16], 12, &[0; 8]);
    }
}
