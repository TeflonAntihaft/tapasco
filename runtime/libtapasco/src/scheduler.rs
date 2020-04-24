use crate::pe::PEId;
use crate::pe::PE;
use crossbeam::deque::{Injector, Steal};
use lockfree::map::Map;
use lockfree::set::Set;
use memmap::MmapMut;
use snafu::ResultExt;
use std::fs::File;
use std::sync::{Arc, Mutex};
use std::thread;

#[derive(Debug, Snafu)]
pub enum Error {
    #[snafu(display("PE Type {} unavailable.", id))]
    PEUnavailable { id: PEId },

    #[snafu(display("PE Type {} is unknown.", id))]
    NoSuchPE { id: PEId },

    #[snafu(display("PE {} is still active. Can't release it.", pe.id()))]
    PEStillActive { pe: PE },

    #[snafu(display("PE Error: {}", source))]
    PEError { source: crate::pe::Error },
}

type Result<T, E = Error> = std::result::Result<T, E>;

#[derive(Debug)]
pub struct Scheduler {
    pes: Map<PEId, Injector<PE>>,
}

impl Scheduler {
    pub fn new(
        pes: &Vec<crate::device::status::Pe>,
        mmap: &Arc<MmapMut>,
        completion: File,
    ) -> Result<Scheduler> {
        let active_pes = Arc::new((Mutex::new(completion), Set::new()));

        let pe_hashed: Map<PEId, Injector<PE>> = Map::new();

        for (i, pe) in pes.iter().enumerate() {
            let the_pe = PE::new(
                i,
                pe.id as PEId,
                pe.offset,
                pe.size,
                pe.name.to_string(),
                mmap.clone(),
                active_pes.clone(),
            );

            match pe_hashed.get(&(pe.id as PEId)) {
                Some(l) => l.val().push(the_pe),
                None => {
                    trace!("New PE type found: {}.", pe.id);
                    let v = Injector::new();
                    v.push(the_pe);
                    pe_hashed.insert(pe.id as PEId, v);
                }
            }
        }

        Ok(Scheduler { pes: pe_hashed })
    }

    pub fn acquire_pe(&self, id: PEId) -> Result<PE> {
        match self.pes.get(&id) {
            Some(l) => loop {
                match l.val().steal() {
                    Steal::Success(pe) => return Ok(pe),
                    Steal::Empty => (),
                    Steal::Retry => (),
                }
                thread::yield_now();
            },
            None => return Err(Error::NoSuchPE { id }),
        }
    }

    pub fn release_pe(&self, pe: PE) -> Result<()> {
        ensure!(!pe.active(), PEStillActive { pe: pe });

        match self.pes.get(&pe.type_id()) {
            Some(l) => l.val().push(pe),
            None => return Err(Error::NoSuchPE { id: *pe.type_id() }),
        }
        Ok(())
    }

    pub fn reset_interrupts(&self) -> Result<()> {
        for v in self.pes.iter() {
            let mut remove_pes = Vec::new();
            let mut maybe_pe = v.val().steal();

            while let Steal::Success(pe) = maybe_pe {
                pe.enable_interrupt().context(PEError)?;
                if pe.interrupt_set().context(PEError)? {
                    pe.reset_interrupt(true).context(PEError)?;
                }
                remove_pes.push(pe);
                maybe_pe = v.val().steal();
            }

            for pe in remove_pes.into_iter() {
                v.val().push(pe);
            }
        }

        Ok(())
    }
}
