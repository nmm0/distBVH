/*
 * distBVH 1.0
 *
 * Copyright 2023 National Technology & Engineering Solutions of Sandia, LLC
 * (NTESS). Under the terms of Contract DE-NA0003525 with NTESS, the U.S.
 * Government retains certain rights in this software.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation and/or
 * other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "collision_object.hpp"
#include "kdop.hpp"
#include "tree.hpp"
#include "util/bits.hpp"
#include "split/split.hpp"
#include "split/mean.hpp"
#include "split/axis.hpp"
#include "debug/assert.hpp"
#include "collision_object/types.hpp"
#include "collision_object/impl.hpp"
#include "collision_object/top_down.hpp"
#include "collision_object/broadphase.hpp"
#include "collision_object/narrowphase.hpp"
#include <unordered_map>
#include <typeinfo>

namespace bvh
{
  using namespace collision_object_impl;
  namespace details
  {
    void set_broadphase_patches( broadphase_patch_collection_type *_coll, broadphase_patch_msg *_msg )
    {
      _coll->patch = std::move( _msg->patch );
      _coll->origin_node = _msg->origin_node;
      _coll->local_idx =  _msg->local_idx;
    }

    void set_narrowphase_patches( narrowphase_patch_collection_type *_coll, narrowphase_patch_msg *_msg )
    {
      // Set data
      _coll->patch_meta = _msg->patch_meta;
      _coll->bytes.resize( _msg->data_size );
      std::memcpy( _coll->bytes.data(), _msg->user_data(), _msg->data_size );
      _coll->origin_node = _msg->origin_node;

      // Reset cache destinations
      _coll->ghost_destinations.clear();
    }
  } // namespace details

  collision_object::collision_object( collision_world &_world, std::size_t _idx, std::size_t _overdecomposition )
    : m_impl{ std::make_unique< impl >( _world, _idx ) }
  {
    bvh_splitting_geom_axis_ = ::vt::theTrace()->registerUserEventColl("bvh_splitting_geom_axis_");
    bvh_splitting_ml_ = ::vt::theTrace()->registerUserEventColl("bvh_splitting_ml_");
    bvh_set_entity_data_impl_ = ::vt::theTrace()->registerUserEventColl("bvh_set_entity_data_impl_");
    bvh_build_trees_ = ::vt::theTrace()->registerUserEventColl("bvh_build_trees_");

    m_impl->overdecomposition = static_cast< int >( _overdecomposition );

    for ( std::size_t i = 0; i < m_impl->overdecomposition; ++i )
    {
      m_impl->chainset.addIndex( vt_index{ i } );
    }

    // Initialize objgroup for per-node data
    ::vt::runInEpochCollective( "collision_object.make_objgroup", [&](){
      m_impl->objgroup = ::vt::theObjGroup()->makeCollective<collision_object_holder>( fmt::vt::format( "collision_object {}", _idx ) );
      m_impl->objgroup.get()->self = this;

      vt::debug( "objgroup make_collective {:x}\n", m_impl->objgroup.getProxy() );
    });
  }

  collision_object::collision_object( collision_object && ) noexcept = default;

  collision_object &collision_object::operator=( collision_object && ) noexcept = default;

  collision_object::~collision_object() = default;

  void collision_object::set_entity_data_impl( const void *_data, std::size_t _element_size )
  {
    const int rank = static_cast< int >( ::vt::theContext()->getNode() );
    const auto od_factor = m_impl->overdecomposition;

    m_impl->num_splits = m_impl->splits.extent( 0 );

    m_impl->local_patches.clear();
    m_impl->local_patches.resize( od_factor );

    always_assert( m_impl->num_splits + 1 == od_factor, "error during splitting process, splits {} do not match od factor {}\n", m_impl->num_splits + 1, od_factor );

    // Preallocate local data buffers. Do this lazily
    m_impl->narrowphase_patch_messages.resize( od_factor, nullptr );
    auto range_policy = Kokkos::RangePolicy< Kokkos::Serial >( 0, od_factor );

    m_impl->m_entity_ptr = static_cast< const unsigned char * >( _data );
    m_impl->m_entity_unit_size = _element_size;

    // Ensure that our update of m_impl->snapshots has finished before reading it here
    Kokkos::fence();

    for ( std::size_t i = 0; i < od_factor; ++i ) {
      const auto sbeg = ( i == 0 ) ? 0 : m_impl->splits_h( i - 1 );
      const auto send = ( i == m_impl->num_splits ) ? m_impl->split_indices_h.extent( 0 ) : m_impl->splits_h( i );
      const std::size_t nelements = send - sbeg;
      m_impl->local_patches[i] = broadphase_patch_type(
        i + rank * od_factor, span< const entity_snapshot >( m_impl->snapshots.data() + sbeg, nelements ) );
    }

    always_assert( m_impl->local_patches.size() == od_factor, "wrong number of patches\n" );
  }

  void collision_object::init_broadphase() const
  {
    m_impl->local_results.clear();
    m_impl->active_narrowphase_indices.clear();
    m_impl->active_narrowphase_local_index.clear();
    m_impl->narrowphase_patch_cache.clear();

    const int rank = static_cast< int >( ::vt::theContext()->getNode() );
    const auto od_factor = m_impl->overdecomposition;

    // Now lazily construct the collection if it's necessary
    auto coll_size = vt_index{ static_cast< std::size_t >( od_factor * ::vt::theContext()->getNumNodes() ) };
    if ( m_impl->broadphase_patch_collection_proxy.getProxy() == ::vt::no_vrt_proxy )
    {
      m_impl->broadphase_patch_collection_proxy = ::vt::makeCollection< broadphase_patch_collection_type >().bounds( coll_size ).bulkInsert().wait();
      m_impl->narrowphase_patch_collection_proxy = ::vt::makeCollection< narrowphase_patch_collection_type >().bounds( coll_size ).bulkInsert().wait();
      m_impl->narrowphase_collection_proxy = ::vt::makeCollection< narrowphase_collection_type >().dynamicMembership( true ).wait();
    }

    // Update the data; od_factor should be identical across nodes
    std::size_t offset = rank * od_factor;
    m_impl->chainset.nextStep( "broadphase_patch_step", [this, rank, offset]( vt_index _local ) {
      auto msg = ::vt::makeMessage< broadphase_patch_msg >();
      msg->patch = m_impl->local_patches.at( _local.x() );
      msg->origin_node = rank;
      msg->local_idx = _local;
      return m_impl->broadphase_patch_collection_proxy[vt_index{ _local.x() + offset }]
      .sendMsg< broadphase_patch_msg, &details::set_broadphase_patches >( msg.get() );
    } );

    // Right now use top down algorithm
    // TODO: insert bottom up algorithm here
    if ( m_impl->build_trees )
    {
      ::vt::trace::TraceScopedEvent scope(bvh_build_trees_);
      // Tree build needs to be done collectively, everyone needs to finish before the next step
      m_impl->chainset.nextStepCollective( "build_tree_step", [this, offset]( vt_index _idx ){
        return collision_object_impl::build_trees_top_down( vt_index{ _idx.x() + offset },
            m_impl->objgroup, m_impl->broadphase_patch_collection_proxy );
      } );
    }

  }

  void
  collision_object::for_each_tree_impl( tree_function &&_fun )
  {
    // note: capture by reference in outer lambda ok, nextStepCollective executes it within scope
    // _fun must be captured by value in the inner lambda since it can go out of scope
    // note that this obviously neeeds to be in scope
    m_impl->chainset.nextStepCollective( "for_each_step", [this, &_fun]( vt_index _idx ){
      return pending_send{ ::vt::no_epoch, [this, _fun, _idx](){
        if ( _idx == vt_index{ 0UL } )  // first index
          _fun( m_impl->tree );
      } };
    } );
  }


  void
  collision_object::for_each_result_impl( std::function< void(const narrowphase_result &) > &&_fun )
  {
    m_impl->chainset.nextStepCollective( "result_step", [this, &_fun]( vt_index _idx ){
      return pending_send{ ::vt::no_epoch, [this, _fun, _idx](){
        if ( _idx == vt_index{ 0UL } )  // first index
        {
          for ( auto &&res : m_impl->local_results )
          {
            _fun( res );
          }
        }
      } };
    } );
  }

  void
  collision_object::broadphase( collision_object &_other )
  {
    const auto od_factor = m_impl->overdecomposition;
    int rank = static_cast< int >( ::vt::theContext()->getNode() );
    std::size_t offset = rank * od_factor;

    m_impl->chainset.nextStepCollective( "start broadphase insertion", [this]( vt_index _local_idx) {
      if ( _local_idx.x() == 0 ) {
        auto msg = ::vt::makeMessage< collision_object_impl::messages::modify_msg >();
        return m_impl->objgroup[::vt::theContext()->getNode()].sendMsg< collision_object_impl::messages::modify_msg, &collision_object_impl::collision_object_holder::begin_narrowphase_modification >( msg );
      } else
        return pending_send{ nullptr };
    } );

    using chainset_type = ::vt::messaging::CollectionChainSet< vt_index >;
    chainset_type::mergeStepCollective( "broadphase_step",m_impl->chainset, _other.m_impl->chainset,
                                       [this, rank, offset, &_other]( vt_index _idx ) {
                                         return collision_object_impl::broadphase(
                                           vt_index{ _idx.x() + offset }, vt_index{ _idx.x() }, rank,
                                           m_impl->broadphase_patch_collection_proxy, m_impl->objgroup,
                                           _other.m_impl->objgroup );
    } );

    m_impl->chainset.nextStepCollective( "finalize broadphase insertion", [this]( vt_index _local_idx) {
      if ( _local_idx.x() == 0 ) {
        auto msg = ::vt::makeMessage< collision_object_impl::messages::modify_msg >();
        return m_impl->objgroup[::vt::theContext()->getNode()].sendMsg< collision_object_impl::messages::modify_msg, &collision_object_impl::collision_object_holder::finish_narrowphase_modification >( msg );
      } else
        return pending_send{ nullptr };
    } );

#ifdef BVH_COPY_ALL_NARROWPHASE_PATCHES
    this->set_all_narrow_patches();
    _other.set_all_narrow_patches();
#else
    this->set_active_narrow_patches();
    _other.set_active_narrow_patches();
#endif
    //
    this->narrowphase(_other);
  }

  void
  collision_object::set_all_narrow_patches(){

    const int rank = static_cast< int >( ::vt::theContext()->getNode() );
    const auto od_factor = m_impl->overdecomposition;

    for ( std::size_t i = 0; i < od_factor; ++i )
      m_impl->narrowphase_patch_messages[i] = m_impl->prepare_local_patch_for_sending( i, rank );

    always_assert( m_impl->local_patches.size() == od_factor,
                  "\n !!! Error during splitting process -- Splits do not match od factor !!!\n\n" );

    const std::size_t offset = rank * od_factor;
    m_impl->chainset.nextStep( "narrowphase_patch_step", [this, offset]( vt_index _local ) {
      auto &msg = m_impl->narrowphase_patch_messages[_local.x()];
      msg->patch_meta = m_impl->local_patches[_local.x()];
      msg->origin_node = ::vt::theContext()->getNode();
      // User data and data_size already filled in
      return m_impl->narrowphase_patch_collection_proxy[vt_index{ _local.x() + offset }]
          .sendMsg< narrowphase_patch_msg, &details::set_narrowphase_patches >( msg.get() );
    } );

  }

  void
  collision_object::set_active_narrow_patches(){
    int rank = static_cast< int >( ::vt::theContext()->getNode() );
    const auto od_factor = m_impl->overdecomposition;
    const std::size_t offset = rank * od_factor;

    m_impl->chainset.nextStepCollective( "set_narrowphase_patches", [this, rank]( vt_index _idx ) {
      if ( _idx.x() == 0 ) {
        auto msg = ::vt::makeMessage< setup_narrowphase_msg >();
        return m_impl->objgroup[rank].sendMsg< setup_narrowphase_msg, &collision_object_impl::collision_object_holder::setup_narrowphase >( msg );
      }
      else {
        return pending_send{ nullptr };
      }
    } );
  }

  void
  collision_object::narrowphase(collision_object &_other ){
    const auto od_factor = m_impl->overdecomposition;
    int rank = static_cast< int >( ::vt::theContext()->getNode() );
    std::size_t offset = rank * od_factor;
    // After the last step, all elements of the narrowphase collection
    // have been inserted

    // We need to activate them
    using chainset_type = ::vt::messaging::CollectionChainSet< vt_index >;
    chainset_type::mergeStepCollective( "activate_narrowphase_step", m_impl->chainset, _other.m_impl->chainset,
    [this]( vt_index _idx ) {
      if ( _idx.x() == 0 )
        return collision_object_impl::activate_narrowphase( _idx, this->m_impl->objgroup );
      else
        return pending_send{ nullptr };
    } );

    // Proceed with narrowphase
    m_impl->chainset.nextStepCollective( "request_ghosts", [this, &_other]( vt_index _idx ){
      if ( _idx.x() == 0 ) {
        return collision_object_impl::request_ghosts( _idx, m_impl->objgroup, _other.m_impl->objgroup );
      } else {
        return pending_send{ nullptr };
      }
    } );

    // TODO: make this just nextStep and cause it to trigger individual narrowphases
    m_impl->chainset.nextStepCollective( "ghost_this", [this, offset]( vt_index _local_idx ){
      return collision_object_impl::ghost( vt_index{ _local_idx.x() + offset},
      m_impl->objgroup,
      m_impl->narrowphase_patch_collection_proxy );
    } );

    m_impl->chainset.nextStepCollective( "ghost_other", [offset, &_other]( vt_index _local_idx ){
      return collision_object_impl::ghost( vt_index{ _local_idx.x() + offset},
      _other.m_impl->objgroup,
      _other.m_impl->narrowphase_patch_collection_proxy );
    } );

    using chainset_type = ::vt::messaging::CollectionChainSet< vt_index >;
    chainset_type::mergeStepCollective( "narrowphase", m_impl->chainset,
    _other.m_impl->chainset, [this]( vt_index _idx ){
      if ( _idx.x() == 0 ) {
        return collision_object_impl::narrowphase( _idx, m_impl->objgroup );
      } else {
        return pending_send{ nullptr };
      }
    } );

    m_impl->chainset.nextStepCollective( "clear_narrowphase_step", [this]( vt_index _idx ){
      if (_idx.x() == 0) {
        return collision_object_impl::clear_narrowphase( _idx, m_impl->objgroup );
      } else {
        return pending_send{ nullptr };
      }
    } );
  }

  void
  collision_object::end_phase()
  {
    m_impl->chainset.phaseDone();
  }

  int
  collision_object::overdecomposition_factor() const noexcept
  {
    return m_impl->overdecomposition;
  }

  std::size_t
  collision_object::id() const noexcept
  {
    return m_impl->collision_idx;
  }

  view< bvh::entity_snapshot * > &
  collision_object::get_snapshots()
  {
    return m_impl->snapshots;
  }

  view< std::size_t * > &
  collision_object::get_split_indices()
  {
    return m_impl->split_indices;
  }

  view< std::size_t * > &
  collision_object::get_splits()
  {
    return m_impl->splits;
  }

  host_view< std::size_t * > &
  collision_object::get_split_indices_h()
  {
    return m_impl->split_indices_h;
  }

  host_view< std::size_t * > &
  collision_object::get_splits_h()
  {
    return m_impl->splits_h;
  }

  span< const patch<> >
  collision_object::local_patches() const noexcept
  {
    return m_impl->local_patches;
  }

  void
  collision_object::initialize_split_indices( const element_permutations &_splits )
  {
    Kokkos::resize( Kokkos::WithoutInitializing, m_impl->split_indices, _splits.indices.size() );
    Kokkos::resize( Kokkos::WithoutInitializing, m_impl->split_indices_h, _splits.indices.size() );
    Kokkos::resize( Kokkos::WithoutInitializing, m_impl->splits, _splits.splits.size() );
    Kokkos::resize( Kokkos::WithoutInitializing, m_impl->splits_h, _splits.splits.size() );

    Kokkos::View< const std::size_t *, bvh::host_execution_space, Kokkos::MemoryTraits< Kokkos::Unmanaged > > indices_view( _splits.indices.data(), _splits.indices.size() );
    Kokkos::View< const std::size_t *, bvh::host_execution_space, Kokkos::MemoryTraits< Kokkos::Unmanaged > > splits_view( _splits.splits.data(), _splits.splits.size() );

    Kokkos::deep_copy( m_impl->splits_h, splits_view );
    Kokkos::deep_copy( m_impl->split_indices_h, indices_view );
  }

} // namespace bvh
