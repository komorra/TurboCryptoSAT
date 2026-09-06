#include "propagator.h"

namespace tcs {

void Propagator::attach(const Cnf& cnf) {
    cnf_ = &cnf;
    value_.assign(static_cast<size_t>(cnf.numVars), 0);
    trail_.clear();
    trail_.reserve(static_cast<size_t>(cnf.numVars));
    satCnt_.assign(cnf.clauseCount(), 0);
    falseCnt_.assign(cnf.clauseCount(), 0);
    qhead_ = 0;
    conflict_ = false;
    satisfied_ = 0;
}

bool Propagator::propagate() {
    const uint32_t* occStart = cnf_->occStart.data();
    const uint32_t* occ = cnf_->occ.data();
    const uint32_t* cstart = cnf_->start.data();
    const Lit* lits = cnf_->lits.data();

    while (qhead_ < trail_.size()) {
        const Lit l = trail_[qhead_++];

        // Clauses holding l become (more) satisfied.
        for (uint32_t i = occStart[static_cast<size_t>(l)], e = occStart[static_cast<size_t>(l) + 1];
             i < e; ++i) {
            const uint32_t c = occ[i];
            if (satCnt_[c]++ == 0) ++satisfied_;
        }

        // Clauses holding ~l lose a literal and may become unit or empty.
        const Lit nl = l ^ 1;
        for (uint32_t i = occStart[static_cast<size_t>(nl)], e = occStart[static_cast<size_t>(nl) + 1];
             i < e; ++i) {
            const uint32_t c = occ[i];
            const uint32_t nf = ++falseCnt_[c];
            if (satCnt_[c] != 0 || conflict_) continue;
            const uint32_t len = cstart[c + 1] - cstart[c];
            if (nf == len) {
                // Keep draining the queue so the counters stay consistent with
                // the trail; undoTo() relies on that when rolling back.
                conflict_ = true;
                continue;
            }
            if (nf + 1 == len) {
                const Lit* b = lits + cstart[c];
                for (uint32_t k = 0; k < len; ++k) {
                    if (litValue(b[k]) == 0) {
                        if (!enqueue(b[k])) conflict_ = true;
                        break;
                    }
                }
            }
        }
    }
    return !conflict_;
}

void Propagator::undoTo(size_t m) {
    const uint32_t* occStart = cnf_->occStart.data();
    const uint32_t* occ = cnf_->occ.data();

    for (size_t i = trail_.size(); i-- > m;) {
        const Lit l = trail_[i];
        if (i < qhead_) {
            for (uint32_t j = occStart[static_cast<size_t>(l)], e = occStart[static_cast<size_t>(l) + 1];
                 j < e; ++j) {
                const uint32_t c = occ[j];
                if (--satCnt_[c] == 0) --satisfied_;
            }
            const Lit nl = l ^ 1;
            for (uint32_t j = occStart[static_cast<size_t>(nl)], e = occStart[static_cast<size_t>(nl) + 1];
                 j < e; ++j) {
                --falseCnt_[occ[j]];
            }
        }
        value_[static_cast<size_t>(l >> 1)] = 0;
    }
    trail_.resize(m);
    if (qhead_ > m) qhead_ = m;
    conflict_ = false;
}

}  // namespace tcs
