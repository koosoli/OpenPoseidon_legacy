//! Diagnostic classification only. This never licenses reuse of a COUNT fact,
//! a cached image, or retirement of geometry.

use super::cull::{MainCountFact, MainCountStatus};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Relation {
    #[default]
    Unknown,
    CountInputsChanged,
    CountEquivalentImageStale,
    CountEquivalentImageCurrent,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Snapshot {
    pub count_inputs_changed: u64,
    pub count_equivalent_image_stale: u64,
    pub count_equivalent_image_current: u64,
}

impl Snapshot {
    pub fn record(&mut self, view: usize, relation: Relation) {
        let bit = 1u64 << view;
        match relation {
            Relation::CountInputsChanged => self.count_inputs_changed |= bit,
            Relation::CountEquivalentImageStale => self.count_equivalent_image_stale |= bit,
            Relation::CountEquivalentImageCurrent => self.count_equivalent_image_current |= bit,
            Relation::Unknown => {}
        }
    }
}

pub fn classify(fact: MainCountFact, same_image: bool, same_cull: bool,
                target: Option<(u32, u64, u64)>, image_epoch: u64,
                current_epoch: u64) -> Relation {
    if !same_image || fact.identity.token == 0 || fact.identity.source_generation == 0 ||
        !matches!((fact.status, fact.count),
            (MainCountStatus::Present, 1..) | (MainCountStatus::Absent, 0)) {
        return Relation::Unknown;
    }
    if image_epoch == 0 || image_epoch != fact.identity.cull_epoch || current_epoch == 0 {
        return Relation::Unknown;
    }
    let Some(target) = target else { return Relation::Unknown; };
    if target != (fact.identity.model_id, fact.identity.source_generation,
                  fact.identity.target_revision) || fact.identity.target_revision == 0 ||
        !same_cull { return Relation::CountInputsChanged; }
    if image_epoch == current_epoch { Relation::CountEquivalentImageCurrent }
    else { Relation::CountEquivalentImageStale }
}

#[cfg(test)]
mod tests {
    use super::*;
    use super::super::cull::MainCountIdentity;

    fn fact(status: MainCountStatus, count: u32) -> MainCountFact {
        MainCountFact { identity: MainCountIdentity { token: 3, model_id: 7,
            source_generation: 9, cull_epoch: 11, target_revision: 2 }, status, count }
    }

    #[test]
    fn equivalent_count_does_not_make_old_image_current() {
        let target = Some((7, 9, 2));
        for answer in [fact(MainCountStatus::Absent, 0), fact(MainCountStatus::Present, 4)] {
            assert_eq!(classify(answer, true, true, target, 11, 12),
                Relation::CountEquivalentImageStale);
            assert_eq!(classify(answer, true, true, target, 11, 11),
                Relation::CountEquivalentImageCurrent);
            assert_eq!(classify(answer, true, false, target, 11, 12),
                Relation::CountInputsChanged);
            assert_eq!(classify(answer, true, true, Some((7, 9, 3)), 11, 12),
                Relation::CountInputsChanged);
            assert_eq!(classify(answer, false, true, target, 11, 12), Relation::Unknown);
            assert_eq!(classify(answer, true, true, None, 11, 12), Relation::Unknown);
        }
    }

    #[test]
    fn no_mapped_count_or_consistent_image_epoch_stays_unknown() {
        let target = Some((7, 9, 2));
        for answer in [fact(MainCountStatus::Unknown, 0), fact(MainCountStatus::Present, 0),
                       fact(MainCountStatus::Absent, 2)] {
            assert_eq!(classify(answer, true, true, target, 11, 12), Relation::Unknown);
        }
        assert_eq!(classify(fact(MainCountStatus::Absent, 0), true, true, target, 10, 12),
            Relation::Unknown);
        assert_eq!(classify(fact(MainCountStatus::Absent, 0), true, true, target, 0, 12),
            Relation::Unknown);
    }
}
