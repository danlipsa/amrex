#ifndef AMREX_PARTICLEUTIL_H_
#define AMREX_PARTICLEUTIL_H_
#include <AMReX_Config.H>

#include <AMReX_IntVect.H>
#include <AMReX_Box.H>
#include <AMReX_Gpu.H>
#include <AMReX_Print.H>
#include <AMReX_Math.H>
#include <AMReX_MFIter.H>
#include <AMReX_ParGDB.H>
#include <AMReX_ParticleTile.H>
#include <AMReX_ParticleBufferMap.H>
#include <AMReX_TypeTraits.H>
#include <AMReX_Scan.H>

#include <limits>

namespace amrex
{

namespace particle_detail {

template <typename F, typename P>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
auto call_f (F const& f, P const& p, amrex::RandomEngine const& engine) noexcept
    -> decltype(f(P{},RandomEngine{}))
{
    return f(p,engine);
}

template <typename F, typename P>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
auto call_f (F const& f, P const& p, amrex::RandomEngine const&) noexcept
    -> decltype(f(P{}))
{
    return f(p);
}

template <typename F, typename SrcData, typename N>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
auto call_f (F const& f, SrcData const& src, N i, amrex::RandomEngine const& engine) noexcept
    -> decltype(f(SrcData{},N{},RandomEngine{}))
{
    return f(src,i,engine);
}

template <typename F, typename SrcData, typename N>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
auto call_f (F const& f, SrcData const& src, N i, amrex::RandomEngine const&) noexcept
    -> decltype(f(SrcData{},N{}))
{
    return f(src,i);
}

template <typename F, typename P, typename T>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
auto call_f (F const& f, P const& p, Array4<T> const& fabarr,
             GpuArray<Real,AMREX_SPACEDIM> const& plo,
             GpuArray<Real,AMREX_SPACEDIM> const& dxi) noexcept
    -> decltype(f(p, fabarr, plo, dxi))
{
    return f(p, fabarr, plo, dxi);
}

template <typename F, typename P, typename T>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
auto call_f (F const& f, P const& p, Array4<T> const& fabarr,
             GpuArray<Real,AMREX_SPACEDIM> const&,
             GpuArray<Real,AMREX_SPACEDIM> const&) noexcept
    -> decltype(f(p, fabarr))
{
    return f(p, fabarr);
}

template <typename F, typename P, typename T>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
auto call_f (F const& f, P& p, Array4<const T> const& fabarr,
             GpuArray<Real,AMREX_SPACEDIM> const& plo,
             GpuArray<Real,AMREX_SPACEDIM> const& dxi) noexcept
    -> decltype(f(p, fabarr, plo, dxi))
{
    return f(p, fabarr, plo, dxi);
}

template <typename F, typename P, typename T>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
auto call_f (F const& f, P& p, Array4<const T> const& fabarr,
             GpuArray<Real,AMREX_SPACEDIM> const&,
             GpuArray<Real,AMREX_SPACEDIM> const&) noexcept
    -> decltype(f(p, fabarr))
{
    return f(p, fabarr);
}

}

/**
 * \brief Returns the number of particles that are more than nGrow cells
 * from the box correspond to the input iterator.
 *
 * \tparam Iterator an AMReX ParticleIterator
 *
 * \param the iterator pointing to the current grid/tile to test
 * \param nGrow the number of grow cells allowed.
 *
 */
template <class Iterator, std::enable_if_t<IsParticleIterator<Iterator>::value, int> foo = 0>
int
numParticlesOutOfRange (Iterator const& pti, int nGrow)
{
    return numParticlesOutOfRange(pti,
                                  IntVect(AMREX_D_DECL(nGrow, nGrow, nGrow)));
}

/**
 * \brief Returns the number of particles that are more than nGrow cells
 * from the box correspond to the input iterator.
 *
 * \tparam Iterator an AMReX ParticleIterator
 *
 * \param the iterator pointing to the current grid/tile to test
 * \param nGrow the number of grow cells allowed.
 *
 */
template <class Iterator, std::enable_if_t<IsParticleIterator<Iterator>::value, int> foo = 0>
int
numParticlesOutOfRange (Iterator const& pti, IntVect nGrow)
{
    using ParticleType = typename Iterator::ContainerType::ParticleType;

    const auto& tile = pti.GetParticleTile();
    const auto np = tile.numParticles();
    const auto& aos = tile.GetArrayOfStructs();
    const auto pstruct = aos().dataPtr();
    const auto& geom = pti.Geom(pti.GetLevel());

    const auto domain = geom.Domain();
    const auto plo = geom.ProbLoArray();
    const auto dxi = geom.InvCellSizeArray();

    Box box = pti.tilebox();
    box.grow(nGrow);

    ReduceOps<ReduceOpSum> reduce_op;
    ReduceData<int> reduce_data(reduce_op);
    using ReduceTuple = typename decltype(reduce_data)::Type;

    reduce_op.eval(np, reduce_data,
    [=] AMREX_GPU_DEVICE (int i) -> ReduceTuple
    {
        const ParticleType& p = pstruct[i];
        if ((p.id() < 0)) return false;
        IntVect iv = IntVect(
            AMREX_D_DECL(int(amrex::Math::floor((p.pos(0)-plo[0])*dxi[0])),
                         int(amrex::Math::floor((p.pos(1)-plo[1])*dxi[1])),
                         int(amrex::Math::floor((p.pos(2)-plo[2])*dxi[2]))));
        iv += domain.smallEnd();
        return !box.contains(iv);
    });
    int hv = amrex::get<0>(reduce_data.value(reduce_op));
    return hv;
}

/**
 * \brief Returns the number of particles that are more than nGrow cells
 * from their assigned box.
 *
 * This version tests over all levels.
 *
 * \tparam PC a type of AMReX particle container.
 *
 * \param pc the particle container to test
 * \param nGrow the number of grow cells allowed.
 *
 */
template <class PC, std::enable_if_t<IsParticleContainer<PC>::value, int> foo = 0>
int
numParticlesOutOfRange (PC const& pc, int nGrow)
{
    return numParticlesOutOfRange(pc, 0, pc.finestLevel(), nGrow);
}

/**
 * \brief Returns the number of particles that are more than nGrow cells
 * from their assigned box.
 *
 * This version tests over all levels.
 *
 * \tparam PC a type of AMReX particle container.
 *
 * \param pc the particle container to test
 * \param nGrow the number of grow cells allowed.
 *
 */
template <class PC, std::enable_if_t<IsParticleContainer<PC>::value, int> foo = 0>
int
numParticlesOutOfRange (PC const& pc, IntVect nGrow)
{
    return numParticlesOutOfRange(pc, 0, pc.finestLevel(), nGrow);
}

/**
 * \brief Returns the number of particles that are more than nGrow cells
 * from their assigned box.
 *
 * This version goes over only the specified levels
 *
 * \tparam PC a type of AMReX particle container.
 *
 * \param pc the particle container to test
 * \param lev_min the minimum level to test
 * \param lev_max the maximum level to test
 * \param nGrow the number of grow cells allowed.
 *
 */
template <class PC, std::enable_if_t<IsParticleContainer<PC>::value, int> foo = 0>
int
numParticlesOutOfRange (PC const& pc, int lev_min, int lev_max, int nGrow)
{
    BL_PROFILE("numParticlesOutOfRange()");

    return numParticlesOutOfRange(pc, lev_min, lev_max,
                                  IntVect(AMREX_D_DECL(nGrow, nGrow, nGrow)));
}

/**
 * \brief Returns the number of particles that are more than nGrow cells
 * from their assigned box.
 *
 * This version goes over only the specified levels
 *
 * \tparam PC a type of AMReX particle container.
 *
 * \param pc the particle container to test
 * \param lev_min the minimum level to test
 * \param lev_max the maximum level to test
 * \param nGrow the number of grow cells allowed.
 *
 */
template <class PC, std::enable_if_t<IsParticleContainer<PC>::value, int> foo = 0>
int
numParticlesOutOfRange (PC const& pc, int lev_min, int lev_max, IntVect nGrow)
{
    BL_PROFILE("numParticlesOutOfRange()");

    using ParIter = typename PC::ParConstIterType;
    int num_wrong = 0;
    for (int lev = lev_min; lev <= lev_max; ++lev)
    {
#ifdef AMREX_USE_OMP
#pragma omp parallel if (Gpu::notInLaunchRegion() && !system::regtest_reduction) reduction(+:num_wrong)
#endif
        for(ParIter pti(pc, lev); pti.isValid(); ++pti)
        {
            num_wrong += numParticlesOutOfRange(pti, nGrow);
        }
    }
    ParallelAllReduce::Sum(num_wrong, ParallelContext::CommunicatorSub());

    return num_wrong;
}

AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
int getTileIndex (const IntVect& iv, const Box& box, const bool a_do_tiling,
                  const IntVect& a_tile_size, Box& tbx)
{
    if (a_do_tiling == false) {
        tbx = box;
        return 0;
    } else {
        //
        // This function must be consistent with FabArrayBase::buildTileArray function!!!
        //
        auto tiling_1d = [](int i, int lo, int hi, int tilesize,
                            int& ntile, int& tileidx, int& tlo, int& thi) {
            int ncells = hi-lo+1;
            ntile = amrex::max(ncells/tilesize, 1);
            int ts_right = ncells/ntile;
            int ts_left  = ts_right+1;
            int nleft = ncells - ntile*ts_right;
            int ii = i - lo;
            int nbndry = nleft*ts_left;
            if (ii < nbndry) {
                tileidx = ii / ts_left; // tiles on the left of nbndry have size of ts_left
                tlo = lo + tileidx * ts_left;
                thi = tlo + ts_left - 1;
            } else {
                tileidx = nleft + (ii-nbndry) / ts_right;  // tiles on the right: ts_right
                tlo = lo + tileidx * ts_right + nleft;
                thi = tlo + ts_right - 1;
            }
        };
        const IntVect& small = box.smallEnd();
        const IntVect& big   = box.bigEnd();
        IntVect ntiles, ivIndex, tilelo, tilehi;

        AMREX_D_TERM(int iv0 = amrex::min(amrex::max(iv[0], small[0]), big[0]);,
                     int iv1 = amrex::min(amrex::max(iv[1], small[1]), big[1]);,
                     int iv2 = amrex::min(amrex::max(iv[2], small[2]), big[2]););

        AMREX_D_TERM(tiling_1d(iv0, small[0], big[0], a_tile_size[0], ntiles[0], ivIndex[0], tilelo[0], tilehi[0]);,
                     tiling_1d(iv1, small[1], big[1], a_tile_size[1], ntiles[1], ivIndex[1], tilelo[1], tilehi[1]);,
                     tiling_1d(iv2, small[2], big[2], a_tile_size[2], ntiles[2], ivIndex[2], tilelo[2], tilehi[2]););

        tbx = Box(tilelo, tilehi);

        return AMREX_D_TERM(ivIndex[0], + ntiles[0]*ivIndex[1], + ntiles[0]*ntiles[1]*ivIndex[2]);
    }
}

AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
int numTilesInBox (const Box& box, const bool a_do_tiling, const IntVect& a_tile_size)
{
    if (a_do_tiling == false) {
        return 1;
    } else {
        //
        // This function must be consistent with FabArrayBase::buildTileArray function!!!
        //
        auto tiling_1d = [](int lo, int hi, int tilesize, int& ntile) {
            int ncells = hi-lo+1;
            ntile = amrex::max(ncells/tilesize, 1);
        };

        const IntVect& small = box.smallEnd();
        const IntVect& big   = box.bigEnd();
        IntVect ntiles;

        AMREX_D_TERM(tiling_1d(small[0], big[0], a_tile_size[0], ntiles[0]);,
                     tiling_1d(small[1], big[1], a_tile_size[1], ntiles[1]);,
                     tiling_1d(small[2], big[2], a_tile_size[2], ntiles[2]););

        return AMREX_D_TERM(ntiles[0], *=ntiles[1], *=ntiles[2]);
    }
}

struct GetParticleBin
{
    GpuArray<Real,AMREX_SPACEDIM> plo;
    GpuArray<Real,AMREX_SPACEDIM> dxi;
    Box domain;
    IntVect bin_size;
    Box box;

    template <typename ParticleType>
    AMREX_GPU_HOST_DEVICE
    unsigned int operator() (const ParticleType& p) const noexcept
    {
        Box tbx;
        auto iv = getParticleCell(p, plo, dxi, domain);
        auto tid = getTileIndex(iv, box, true, bin_size, tbx);
        return static_cast<unsigned int>(tid);
    }
};

template <typename P>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
IntVect getParticleCell (P const& p,
                         amrex::GpuArray<amrex::Real,AMREX_SPACEDIM> const& plo,
                         amrex::GpuArray<amrex::Real,AMREX_SPACEDIM> const& dxi,
                         const Box& domain) noexcept
{
    IntVect iv = IntVect(
        AMREX_D_DECL(int(amrex::Math::floor((p.pos(0)-plo[0])*dxi[0])),
                     int(amrex::Math::floor((p.pos(1)-plo[1])*dxi[1])),
                     int(amrex::Math::floor((p.pos(2)-plo[2])*dxi[2]))));
    iv += domain.smallEnd();
    return iv;
}

template <typename P>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
int getParticleGrid (P const& p, amrex::Array4<int> const& mask,
                     amrex::GpuArray<amrex::Real,AMREX_SPACEDIM> const& plo,
                     amrex::GpuArray<amrex::Real,AMREX_SPACEDIM> const& dxi,
                     const Box& domain) noexcept
{
    if (p.id() < 0) return -1;
    IntVect iv = getParticleCell(p, plo, dxi, domain);
    return mask(iv);
}

template <typename P>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
bool enforcePeriodic (P& p,
                      amrex::GpuArray<amrex::Real,AMREX_SPACEDIM> const& plo,
                      amrex::GpuArray<amrex::Real,AMREX_SPACEDIM> const& phi,
                      amrex::GpuArray<int,AMREX_SPACEDIM> const& is_per) noexcept
{
    bool shifted = false;
    for (int idim = 0; idim < AMREX_SPACEDIM; ++idim)
    {
        if (! is_per[idim]) continue;
        if (p.pos(idim) >= phi[idim]) {
            while (p.pos(idim) >= phi[idim]) {
                p.pos(idim) -= static_cast<ParticleReal>(phi[idim] - plo[idim]);
            }
            // clamp to avoid precision issues;
            if (p.pos(idim) < plo[idim]) p.pos(idim) = static_cast<ParticleReal>(plo[idim]);
            shifted = true;
        }
        else if (p.pos(idim) < plo[idim]) {
            while (p.pos(idim) < plo[idim]) {
                p.pos(idim) += static_cast<ParticleReal>(phi[idim] - plo[idim]);
            }
            // clamp to avoid precision issues;
            if (p.pos(idim) == phi[idim]) p.pos(idim) = static_cast<ParticleReal>(plo[idim]);
            if (p.pos(idim) > phi[idim]) p.pos(idim) = std::nextafter( (amrex::ParticleReal) phi[idim], (amrex::ParticleReal) plo[idim]);
            shifted = true;
        }
        AMREX_ASSERT( (p.pos(idim) >= plo[idim] ) && ( p.pos(idim) < phi[idim] ));
    }

    return shifted;
}

#if defined(AMREX_USE_GPU)

template <typename PTile, typename PLocator>
int
partitionParticlesByDest (PTile& ptile, const PLocator& ploc, const ParticleBufferMap& pmap,
                          const Geometry& geom, int lev, int gid, int /*tid*/,
                          int lev_min, int lev_max, int nGrow)
{
    const auto plo    = geom.ProbLoArray();
    const auto phi    = geom.ProbHiArray();
    const auto is_per = geom.isPeriodicArray();

    auto& aos = ptile.GetArrayOfStructs();
    const int np = aos.numParticles();

    if (np == 0) return 0;

    auto getPID = pmap.getPIDFunctor();
    auto p_ptr = &(aos[0]);

    int pid = ParallelContext::MyProcSub();
    constexpr int chunk_size = 256*256*256;
    int num_chunks = std::max(1, (np + (chunk_size - 1)) / chunk_size);

    PTile ptile_tmp;
    ptile_tmp.define(ptile.NumRuntimeRealComps(), ptile.NumRuntimeIntComps());
    ptile_tmp.resize(std::min(np, chunk_size));

    auto src_data = ptile.getParticleTileData();
    auto dst_data = ptile_tmp.getParticleTileData();

    int last_offset = 0;
    for (int ichunk = 0; ichunk < num_chunks; ++ichunk)
    {
        int this_offset = ichunk*chunk_size;
        int this_chunk_size = std::min(chunk_size, np - this_offset);

        int num_stay;
        {
            auto particle_stays = [=] AMREX_GPU_DEVICE (int i) -> int
            {
                int assigned_grid;
                int assigned_lev;

                auto& p = p_ptr[i+this_offset];

                if (p.id() < 0 )
                {
                    assigned_grid = -1;
                    assigned_lev  = -1;
                }
                else
                {
                    auto p_prime = p;
                    enforcePeriodic(p_prime, plo, phi, is_per);
                    auto tup_prime = ploc(p_prime, lev_min, lev_max, nGrow);
                    assigned_grid = amrex::get<0>(tup_prime);
                    assigned_lev  = amrex::get<1>(tup_prime);
                    if (assigned_grid >= 0)
                    {
                      AMREX_D_TERM(p.pos(0) = p_prime.pos(0);,
                                   p.pos(1) = p_prime.pos(1);,
                                   p.pos(2) = p_prime.pos(2););
                    }
                    else if (lev_min > 0)
                    {
                      auto tup = ploc(p, lev_min, lev_max, nGrow);
                      assigned_grid = amrex::get<0>(tup);
                      assigned_lev  = amrex::get<1>(tup);
                    }
                }

                return ((assigned_grid == gid) && (assigned_lev == lev) && (getPID(lev, gid) == pid));
            };

            num_stay = Scan::PrefixSum<int> (this_chunk_size,
                          [=] AMREX_GPU_DEVICE (int i) -> int
                          {
                              return particle_stays(i);
                          },
                          [=] AMREX_GPU_DEVICE (int i, int const& s)
                          {
                              int src_i = i + this_offset;
                              int dst_i = particle_stays(i) ? s : this_chunk_size-1-(i-s);
                              copyParticle(dst_data, src_data, src_i, dst_i);
                          },
                          Scan::Type::exclusive);
        }

        if (num_chunks == 1)
        {
            ptile.swap(ptile_tmp);
        }
        else
        {
            AMREX_FOR_1D(this_chunk_size, i,
                         {
                             copyParticle(src_data, dst_data, i, i + this_offset);
                         });
        }

        if ( ichunk > 0 )
        {
            int num_swap = std::min(this_offset - last_offset, num_stay);
            AMREX_FOR_1D( num_swap, i,
            {
                swapParticle(src_data, src_data, last_offset + i,
                             this_offset + num_stay - 1 - i);
            });
        }

        last_offset += num_stay;
    }

    return last_offset;
}

#endif

IntVect computeRefFac (const ParGDBBase* a_gdb, int src_lev, int lev);

Vector<int> computeNeighborProcs (const ParGDBBase* a_gdb, int ngrow);

namespace particle_detail
{
template <typename C>
void clearEmptyEntries (C& c)
{
    for (auto c_it = c.begin(); c_it != c.end(); /* no ++ */)
    {
        if (c_it->second.empty()) { c.erase(c_it++); }
        else { ++c_it; }
    }
}
}

#ifdef AMREX_USE_HDF5_ASYNC
void async_vol_es_wait_particle();
void async_vol_es_wait_close_particle();
#endif
}

#endif // include guard
