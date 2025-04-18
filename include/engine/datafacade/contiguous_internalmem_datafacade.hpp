#ifndef CONTIGUOUS_INTERNALMEM_DATAFACADE_HPP
#define CONTIGUOUS_INTERNALMEM_DATAFACADE_HPP

#include "engine/datafacade/algorithm_datafacade.hpp"
#include "engine/datafacade/contiguous_block_allocator.hpp"
#include "engine/datafacade/datafacade_base.hpp"

#include "engine/algorithm.hpp"
#include "engine/approach.hpp"
#include "engine/geospatial_query.hpp"

#include "storage/shared_datatype.hpp"
#include "storage/shared_memory_ownership.hpp"
#include "storage/view_factory.hpp"

#include "util/exception.hpp"
#include "util/exception_utils.hpp"
#include "util/log.hpp"

#include <boost/assert.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace osrm::engine::datafacade
{

static const std::string DATASET_TURN_DATA = "TurnData";
static const std::string DATASET_TURN_LANE_DATA = "NameLaneData";
static const std::string DATASET_NAME_DATA = "NameData";
static const std::string DATASET_INTERSECTION_BEARINGS = "IntersectionBearings";
static const std::string DATASET_ENTRY_CLASS = "EntryClass";

/**
 * Macro is not ideal. But without it we either have to:
 * a) Write this boiler-plate for every usage of an optional dataset.
 * b) Convert to a function and add lots of polluting NOLINT(bugprone-unchecked-optional-access)
 * comments. This macro keeps the API code readable.
 */
#define CHECK_DATASET_DISABLED(val, dataset)                                                       \
    {                                                                                              \
        if (!(val))                                                                                \
        {                                                                                          \
            throw osrm::util::DisabledDatasetException((dataset));                                 \
        }                                                                                          \
    }

template <typename AlgorithmT> class ContiguousInternalMemoryAlgorithmDataFacade;

template <>
class ContiguousInternalMemoryAlgorithmDataFacade<CH> : public datafacade::AlgorithmDataFacade<CH>
{
  private:
    using QueryGraph = util::FilteredGraphView<contractor::QueryGraphView>;
    using GraphNode = QueryGraph::NodeArrayEntry;
    using GraphEdge = QueryGraph::EdgeArrayEntry;

    QueryGraph m_query_graph;

    // allocator that keeps the allocation data
    std::shared_ptr<ContiguousBlockAllocator> allocator;

  public:
    ContiguousInternalMemoryAlgorithmDataFacade(
        std::shared_ptr<ContiguousBlockAllocator> allocator_,
        const std::string &metric_name,
        std::size_t exclude_index)
        : allocator(std::move(allocator_))
    {
        InitializeInternalPointers(allocator->GetIndex(), metric_name, exclude_index);
    }

    void InitializeInternalPointers(const storage::SharedDataIndex &index,
                                    const std::string &metric_name,
                                    const std::size_t exclude_index)
    {
        m_query_graph =
            make_filtered_graph_view(index, "/ch/metrics/" + metric_name, exclude_index);
    }

    // search graph access
    unsigned GetNumberOfNodes() const override final { return m_query_graph.GetNumberOfNodes(); }

    unsigned GetNumberOfEdges() const override final { return m_query_graph.GetNumberOfEdges(); }

    unsigned GetOutDegree(const NodeID edge_based_node_id) const override final
    {
        return m_query_graph.GetOutDegree(edge_based_node_id);
    }

    NodeID GetTarget(const EdgeID edge_based_edge_id) const override final
    {
        return m_query_graph.GetTarget(edge_based_edge_id);
    }

    const EdgeData &GetEdgeData(const EdgeID edge_based_edge_id) const override final
    {
        return m_query_graph.GetEdgeData(edge_based_edge_id);
    }

    EdgeRange GetAdjacentEdgeRange(const NodeID edge_based_node_id) const override final
    {
        return m_query_graph.GetAdjacentEdgeRange(edge_based_node_id);
    }

    // searches for a specific edge
    EdgeID FindEdge(const NodeID edge_based_node_from,
                    const NodeID edge_based_node_to) const override final
    {
        return m_query_graph.FindEdge(edge_based_node_from, edge_based_node_to);
    }

    EdgeID FindEdgeInEitherDirection(const NodeID edge_based_node_from,
                                     const NodeID edge_based_node_to) const override final
    {
        return m_query_graph.FindEdgeInEitherDirection(edge_based_node_from, edge_based_node_to);
    }

    EdgeID FindEdgeIndicateIfReverse(const NodeID edge_based_node_from,
                                     const NodeID edge_based_node_to,
                                     bool &result) const override final
    {
        return m_query_graph.FindEdgeIndicateIfReverse(
            edge_based_node_from, edge_based_node_to, result);
    }

    EdgeID
    FindSmallestEdge(const NodeID edge_based_node_from,
                     const NodeID edge_based_node_to,
                     const std::function<bool(const EdgeData &)> &filter) const override final
    {
        return m_query_graph.FindSmallestEdge(edge_based_node_from, edge_based_node_to, filter);
    }
};

/**
 * This base class implements the Datafacade interface for accessing
 * data that's stored in a single large block of memory (RAM).
 *
 * In this case "internal memory" refers to RAM - as opposed to "external memory",
 * which usually refers to disk.
 */
class ContiguousInternalMemoryDataFacadeBase : public BaseDataFacade
{
  private:
    using super = BaseDataFacade;
    using RTreeLeaf = super::RTreeLeaf;
    using SharedRTree = util::StaticRTree<RTreeLeaf, storage::Ownership::View>;
    using SharedGeospatialQuery = GeospatialQuery<SharedRTree, BaseDataFacade>;

    extractor::ClassData exclude_mask;
    extractor::ProfileProperties *m_profile_properties;
    extractor::Datasources *m_datasources;

    std::uint32_t m_check_sum;
    std::string_view m_data_timestamp;
    util::vector_view<util::Coordinate> m_coordinate_list;
    extractor::PackedOSMIDsView m_osmnodeid_list;
    std::optional<util::vector_view<std::uint32_t>> m_lane_description_offsets;
    std::optional<util::vector_view<extractor::TurnLaneType::Mask>> m_lane_description_masks;
    util::vector_view<TurnPenalty> m_turn_weight_penalties;
    util::vector_view<TurnPenalty> m_turn_duration_penalties;
    extractor::SegmentDataView segment_data;
    extractor::EdgeBasedNodeDataView edge_based_node_data;
    std::optional<guidance::TurnDataView> turn_data;

    std::optional<util::vector_view<util::guidance::LaneTupleIdPair>> m_lane_tuple_id_pairs;

    util::vector_view<extractor::StorageManeuverOverride> m_maneuver_overrides;
    util::vector_view<NodeID> m_maneuver_override_node_sequences;

    SharedRTree m_static_rtree;
    std::unique_ptr<SharedGeospatialQuery> m_geospatial_query;
    std::filesystem::path file_index_path;

    std::optional<extractor::IntersectionBearingsView> intersection_bearings_view;

    std::optional<extractor::NameTableView> m_name_table;
    // the look-up table for entry classes. An entry class lists the possibility of entry for all
    // available turns. Such a class id is stored with every edge.
    std::optional<util::vector_view<util::guidance::EntryClass>> m_entry_class_table;

    // allocator that keeps the allocation data
    std::shared_ptr<ContiguousBlockAllocator> allocator;

    bool isIndexed(const storage::SharedDataIndex &index, const std::string &name)
    {
        bool result = false;
        index.List(name,
                   boost::make_function_output_iterator([&](const auto &) { result = true; }));
        return result;
    }

    void InitializeInternalPointers(const storage::SharedDataIndex &index,
                                    const std::string &metric_name,
                                    const std::size_t exclude_index)
    {
        // TODO: For multi-metric support we need to have separate exclude classes per metric
        (void)metric_name;

        m_profile_properties =
            index.GetBlockPtr<extractor::ProfileProperties>("/common/properties");

        exclude_mask = m_profile_properties->excludable_classes[exclude_index];

        // We no longer use "/common/connectivity_checksum", as osrm.edges is an optional dataset.
        // Instead, we load the value from the MLD or CH graph, whichever is loaded.
        if (isIndexed(index, "/mld/connectivity_checksum"))
        {
            m_check_sum = *index.GetBlockPtr<std::uint32_t>("/mld/connectivity_checksum");
        }
        else
        {
            BOOST_ASSERT(isIndexed(index, "/ch/connectivity_checksum"));
            m_check_sum = *index.GetBlockPtr<std::uint32_t>("/ch/connectivity_checksum");
        }

        m_data_timestamp = make_timestamp_view(index, "/common/timestamp");

        std::tie(m_coordinate_list, m_osmnodeid_list) =
            make_nbn_data_view(index, "/common/nbn_data");

        m_static_rtree = make_search_tree_view(index, "/common/rtree");
        m_geospatial_query.reset(
            new SharedGeospatialQuery(m_static_rtree, m_coordinate_list, *this));

        edge_based_node_data = make_ebn_data_view(index, "/common/ebg_node_data");

        if (isIndexed(index, "/common/turn_data"))
        {
            turn_data = make_turn_data_view(index, "/common/turn_data");
        }

        if (isIndexed(index, "/common/names"))
        {
            m_name_table = make_name_table_view(index, "/common/names");
        }

        if (isIndexed(index, "/common/turn_lanes"))
        {
            std::tie(m_lane_description_offsets, m_lane_description_masks) =
                make_turn_lane_description_views(index, "/common/turn_lanes");

            m_lane_tuple_id_pairs = make_lane_data_view(index, "/common/turn_lanes");
        }

        m_turn_weight_penalties = make_turn_weight_view(index, "/common/turn_penalty");
        m_turn_duration_penalties = make_turn_duration_view(index, "/common/turn_penalty");

        segment_data = make_segment_data_view(index, "/common/segment_data");

        m_datasources = index.GetBlockPtr<extractor::Datasources>("/common/data_sources_names");

        if (isIndexed(index, "/common/intersection_bearings"))
        {
            intersection_bearings_view =
                make_intersection_bearings_view(index, "/common/intersection_bearings");
            m_entry_class_table = make_entry_classes_view(index, "/common/entry_classes");
        }

        std::tie(m_maneuver_overrides, m_maneuver_override_node_sequences) =
            make_maneuver_overrides_views(index, "/common/maneuver_overrides");
    }

  public:
    // allows switching between process_memory/shared_memory datafacade, based on the type of
    // allocator
    ContiguousInternalMemoryDataFacadeBase(std::shared_ptr<ContiguousBlockAllocator> allocator_,
                                           const std::string &metric_name,
                                           const std::size_t exclude_index)
        : allocator(std::move(allocator_))
    {
        InitializeInternalPointers(allocator->GetIndex(), metric_name, exclude_index);
    }

    // node and edge information access
    util::Coordinate GetCoordinateOfNode(const NodeID node_based_node_id) const override final
    {
        return m_coordinate_list[node_based_node_id];
    }

    OSMNodeID GetOSMNodeIDOfNode(const NodeID node_based_node_id) const override final
    {
        return m_osmnodeid_list[node_based_node_id];
    }

    NodeForwardRange GetUncompressedForwardGeometry(const PackedGeometryID id) const override final
    {
        return segment_data.GetForwardGeometry(id);
    }

    NodeReverseRange GetUncompressedReverseGeometry(const PackedGeometryID id) const override final
    {
        return segment_data.GetReverseGeometry(id);
    }

    DurationForwardRange
    GetUncompressedForwardDurations(const PackedGeometryID id) const override final
    {
        return segment_data.GetForwardDurations(id);
    }

    DurationReverseRange
    GetUncompressedReverseDurations(const PackedGeometryID id) const override final
    {
        return segment_data.GetReverseDurations(id);
    }

    WeightForwardRange GetUncompressedForwardWeights(const PackedGeometryID id) const override final
    {
        return segment_data.GetForwardWeights(id);
    }

    WeightReverseRange GetUncompressedReverseWeights(const PackedGeometryID id) const override final
    {
        return segment_data.GetReverseWeights(id);
    }

    // Returns the data source ids that were used to supply the edge
    // weights.
    DatasourceForwardRange
    GetUncompressedForwardDatasources(const PackedGeometryID id) const override final
    {
        return segment_data.GetForwardDatasources(id);
    }

    // Returns the data source ids that were used to supply the edge
    // weights.
    DatasourceReverseRange
    GetUncompressedReverseDatasources(const PackedGeometryID id) const override final
    {
        return segment_data.GetReverseDatasources(id);
    }

    TurnPenalty GetWeightPenaltyForEdgeID(const EdgeID edge_based_edge_id) const override final
    {
        BOOST_ASSERT(m_turn_weight_penalties.size() > edge_based_edge_id);
        return m_turn_weight_penalties[edge_based_edge_id];
    }

    TurnPenalty GetDurationPenaltyForEdgeID(const EdgeID edge_based_edge_id) const override final
    {
        BOOST_ASSERT(m_turn_duration_penalties.size() > edge_based_edge_id);
        return m_turn_duration_penalties[edge_based_edge_id];
    }

    osrm::guidance::TurnInstruction
    GetTurnInstructionForEdgeID(const EdgeID edge_based_edge_id) const override final
    {
        CHECK_DATASET_DISABLED(turn_data, DATASET_TURN_DATA);
        return turn_data->GetTurnInstruction(edge_based_edge_id);
    }

    std::vector<RTreeLeaf> GetEdgesInBox(const util::Coordinate south_west,
                                         const util::Coordinate north_east) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());
        const util::RectangleInt2D bbox{
            south_west.lon, north_east.lon, south_west.lat, north_east.lat};
        return m_geospatial_query->Search(bbox);
    }

    std::vector<PhantomNodeWithDistance>
    NearestPhantomNodesInRange(const util::Coordinate input_coordinate,
                               const double max_distance,
                               const std::optional<Bearing> bearing,
                               const Approach approach,
                               const bool use_all_edges) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodes(
            input_coordinate, approach, max_distance, bearing, use_all_edges);
    }

    std::vector<PhantomNodeWithDistance>
    NearestPhantomNodes(const util::Coordinate input_coordinate,
                        const size_t max_results,
                        const std::optional<double> max_distance,
                        const std::optional<Bearing> bearing,
                        const Approach approach) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodes(
            input_coordinate, approach, max_results, max_distance, bearing, std::nullopt);
    }

    PhantomCandidateAlternatives
    NearestCandidatesWithAlternativeFromBigComponent(const util::Coordinate input_coordinate,
                                                     const std::optional<double> max_distance,
                                                     const std::optional<Bearing> bearing,
                                                     const Approach approach,
                                                     const bool use_all_edges) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestCandidatesWithAlternativeFromBigComponent(
            input_coordinate, approach, max_distance, bearing, use_all_edges);
    }

    std::uint32_t GetCheckSum() const override final { return m_check_sum; }

    std::string GetTimestamp() const override final
    {
        return std::string(m_data_timestamp.begin(), m_data_timestamp.end());
    }

    GeometryID GetGeometryIndex(const NodeID edge_based_node_id) const override final
    {
        return edge_based_node_data.GetGeometryID(edge_based_node_id);
    }

    ComponentID GetComponentID(const NodeID edge_based_node_id) const override final
    {
        return edge_based_node_data.GetComponentID(edge_based_node_id);
    }

    extractor::TravelMode GetTravelMode(const NodeID edge_based_node_id) const override final
    {
        return edge_based_node_data.GetTravelMode(edge_based_node_id);
    }

    extractor::ClassData GetClassData(const NodeID edge_based_node_id) const override final
    {
        return edge_based_node_data.GetClassData(edge_based_node_id);
    }

    bool ExcludeNode(const NodeID edge_based_node_id) const override final
    {
        return (edge_based_node_data.GetClassData(edge_based_node_id) & exclude_mask) > 0;
    }

    std::vector<std::string> GetClasses(const extractor::ClassData class_data) const override final
    {
        auto indexes = extractor::getClassIndexes(class_data);
        std::vector<std::string> classes(indexes.size());
        std::transform(indexes.begin(),
                       indexes.end(),
                       classes.begin(),
                       [this](const auto index)
                       { return m_profile_properties->GetClassName(index); });

        return classes;
    }

    NameID GetNameIndex(const NodeID edge_based_node_id) const override final
    {
        return edge_based_node_data.GetNameID(edge_based_node_id);
    }

    std::string_view GetNameForID(const NameID id) const override final
    {
        CHECK_DATASET_DISABLED(m_name_table, DATASET_NAME_DATA);
        return m_name_table->GetNameForID(id);
    }

    std::string_view GetRefForID(const NameID id) const override final
    {
        CHECK_DATASET_DISABLED(m_name_table, DATASET_NAME_DATA);
        return m_name_table->GetRefForID(id);
    }

    std::string_view GetPronunciationForID(const NameID id) const override final
    {
        CHECK_DATASET_DISABLED(m_name_table, DATASET_NAME_DATA);
        return m_name_table->GetPronunciationForID(id);
    }

    std::string_view GetDestinationsForID(const NameID id) const override final
    {
        CHECK_DATASET_DISABLED(m_name_table, DATASET_NAME_DATA);
        return m_name_table->GetDestinationsForID(id);
    }

    std::string_view GetExitsForID(const NameID id) const override final
    {
        CHECK_DATASET_DISABLED(m_name_table, DATASET_NAME_DATA);
        return m_name_table->GetExitsForID(id);
    }

    std::string_view GetDatasourceName(const DatasourceID id) const override final
    {
        return m_datasources->GetSourceName(id);
    }

    bool GetContinueStraightDefault() const override final
    {
        return m_profile_properties->continue_straight_at_waypoint;
    }

    double GetMapMatchingMaxSpeed() const override final
    {
        return m_profile_properties->max_speed_for_map_matching;
    }

    const char *GetWeightName() const override final { return m_profile_properties->weight_name; }

    unsigned GetWeightPrecision() const override final
    {
        return m_profile_properties->weight_precision;
    }

    double GetWeightMultiplier() const override final
    {
        return m_profile_properties->GetWeightMultiplier();
    }

    util::guidance::BearingClass
    GetBearingClass(const NodeID node_based_node_id) const override final
    {
        CHECK_DATASET_DISABLED(intersection_bearings_view, DATASET_INTERSECTION_BEARINGS);
        return intersection_bearings_view->GetBearingClass(node_based_node_id);
    }

    guidance::TurnBearing PreTurnBearing(const EdgeID edge_based_edge_id) const override final
    {
        CHECK_DATASET_DISABLED(turn_data, DATASET_TURN_DATA);
        return turn_data->GetPreTurnBearing(edge_based_edge_id);
    }
    guidance::TurnBearing PostTurnBearing(const EdgeID edge_based_edge_id) const override final
    {
        CHECK_DATASET_DISABLED(turn_data, DATASET_TURN_DATA);
        return turn_data->GetPostTurnBearing(edge_based_edge_id);
    }

    util::guidance::EntryClass GetEntryClass(const EdgeID edge_based_edge_id) const override final
    {
        CHECK_DATASET_DISABLED(m_entry_class_table, DATASET_ENTRY_CLASS);
        CHECK_DATASET_DISABLED(turn_data, DATASET_TURN_DATA);

        auto entry_class_id = turn_data->GetEntryClassID(edge_based_edge_id);
        return m_entry_class_table->at(entry_class_id);
    }

    bool HasLaneData(const EdgeID edge_based_edge_id) const override final
    {
        CHECK_DATASET_DISABLED(turn_data, DATASET_TURN_DATA);
        return turn_data->HasLaneData(edge_based_edge_id);
    }

    util::guidance::LaneTupleIdPair
    GetLaneData(const EdgeID edge_based_edge_id) const override final
    {
        CHECK_DATASET_DISABLED(turn_data, DATASET_TURN_DATA);
        CHECK_DATASET_DISABLED(m_lane_tuple_id_pairs, DATASET_TURN_LANE_DATA);

        BOOST_ASSERT(HasLaneData(edge_based_edge_id));
        return m_lane_tuple_id_pairs->at(turn_data->GetLaneDataID(edge_based_edge_id));
    }

    extractor::TurnLaneDescription
    GetTurnDescription(const LaneDescriptionID lane_description_id) const override final
    {
        CHECK_DATASET_DISABLED(m_lane_description_offsets, DATASET_TURN_LANE_DATA);
        CHECK_DATASET_DISABLED(m_lane_description_masks, DATASET_TURN_LANE_DATA);

        if (lane_description_id == INVALID_LANE_DESCRIPTIONID)
            return {};
        else
            return extractor::TurnLaneDescription(
                m_lane_description_masks->begin() +
                    m_lane_description_offsets->at(lane_description_id),
                m_lane_description_masks->begin() +
                    m_lane_description_offsets->at(lane_description_id + 1));
    }

    bool IsLeftHandDriving(const NodeID edge_based_node_id) const override final
    {
        // TODO: can be moved to a data block indexed by GeometryID
        return edge_based_node_data.IsLeftHandDriving(edge_based_node_id);
    }

    bool IsSegregated(const NodeID edge_based_node_id) const override final
    {
        return edge_based_node_data.IsSegregated(edge_based_node_id);
    }

    std::vector<extractor::ManeuverOverride>
    GetOverridesThatStartAt(const NodeID edge_based_node_id) const override final
    {
        std::vector<extractor::ManeuverOverride> results;

        // heterogeneous comparison:
        struct Comp
        {
            bool operator()(const extractor::StorageManeuverOverride &s, NodeID i) const
            {
                return s.start_node < i;
            }
            bool operator()(NodeID i, const extractor::StorageManeuverOverride &s) const
            {
                return i < s.start_node;
            }
        };

        auto found_range = std::equal_range(
            m_maneuver_overrides.begin(), m_maneuver_overrides.end(), edge_based_node_id, Comp{});

        results.reserve(std::distance(found_range.first, found_range.second));

        std::for_each(found_range.first,
                      found_range.second,
                      [&](const auto &override)
                      {
                          std::vector<NodeID> sequence(m_maneuver_override_node_sequences.begin() +
                                                           override.node_sequence_offset_begin,
                                                       m_maneuver_override_node_sequences.begin() +
                                                           override.node_sequence_offset_end);
                          results.push_back(extractor::ManeuverOverride{std::move(sequence),
                                                                        override.instruction_node,
                                                                        override.override_type,
                                                                        override.direction});
                      });
        return results;
    }
};

template <typename AlgorithmT> class ContiguousInternalMemoryDataFacade;

template <>
class ContiguousInternalMemoryDataFacade<CH>
    : public ContiguousInternalMemoryDataFacadeBase,
      public ContiguousInternalMemoryAlgorithmDataFacade<CH>
{
  public:
    ContiguousInternalMemoryDataFacade(const std::shared_ptr<ContiguousBlockAllocator> &allocator,
                                       const std::string &metric_name,
                                       const std::size_t exclude_index)
        : ContiguousInternalMemoryDataFacadeBase(allocator, metric_name, exclude_index),
          ContiguousInternalMemoryAlgorithmDataFacade<CH>(allocator, metric_name, exclude_index)
    {
    }
};

template <> class ContiguousInternalMemoryAlgorithmDataFacade<MLD> : public AlgorithmDataFacade<MLD>
{
    // MLD data
    partitioner::MultiLevelPartitionView mld_partition;
    partitioner::CellStorageView mld_cell_storage;
    customizer::CellMetricView mld_cell_metric;
    using QueryGraph = customizer::MultiLevelEdgeBasedGraphView;
    using GraphNode = QueryGraph::NodeArrayEntry;
    using GraphEdge = QueryGraph::EdgeArrayEntry;

    QueryGraph query_graph;

    void InitializeInternalPointers(const storage::SharedDataIndex &index,
                                    const std::string &metric_name,
                                    const std::size_t exclude_index)
    {
        mld_partition = make_partition_view(index, "/mld/multilevelpartition");
        mld_cell_metric =
            make_filtered_cell_metric_view(index, "/mld/metrics/" + metric_name, exclude_index);
        mld_cell_storage = make_cell_storage_view(index, "/mld/cellstorage");
        query_graph = make_multi_level_graph_view(index, "/mld/multilevelgraph");
    }

    // allocator that keeps the allocation data
    std::shared_ptr<ContiguousBlockAllocator> allocator;

  public:
    ContiguousInternalMemoryAlgorithmDataFacade(
        std::shared_ptr<ContiguousBlockAllocator> allocator_,
        const std::string &metric_name,
        const std::size_t exclude_index)
        : allocator(std::move(allocator_))
    {
        InitializeInternalPointers(allocator->GetIndex(), metric_name, exclude_index);
    }

    const partitioner::MultiLevelPartitionView &GetMultiLevelPartition() const override
    {
        return mld_partition;
    }

    const partitioner::CellStorageView &GetCellStorage() const override { return mld_cell_storage; }

    const customizer::CellMetricView &GetCellMetric() const override { return mld_cell_metric; }

    // search graph access
    unsigned GetNumberOfNodes() const override final { return query_graph.GetNumberOfNodes(); }

    unsigned GetMaxBorderNodeID() const override final { return query_graph.GetMaxBorderNodeID(); }

    unsigned GetNumberOfEdges() const override final { return query_graph.GetNumberOfEdges(); }

    unsigned GetOutDegree(const NodeID edge_based_node_id) const override final
    {
        return query_graph.GetOutDegree(edge_based_node_id);
    }

    EdgeRange GetAdjacentEdgeRange(const NodeID edge_based_node_id) const override final
    {
        return query_graph.GetAdjacentEdgeRange(edge_based_node_id);
    }

    EdgeWeight GetNodeWeight(const NodeID edge_based_node_id) const override final
    {
        return query_graph.GetNodeWeight(edge_based_node_id);
    }

    EdgeDuration GetNodeDuration(const NodeID edge_based_node_id) const override final
    {
        return query_graph.GetNodeDuration(edge_based_node_id);
    }

    EdgeDistance GetNodeDistance(const NodeID edge_based_node_id) const override final
    {
        return query_graph.GetNodeDistance(edge_based_node_id);
    }

    bool IsForwardEdge(const NodeID edge_based_node_id) const override final
    {
        return query_graph.IsForwardEdge(edge_based_node_id);
    }

    bool IsBackwardEdge(const NodeID edge_based_node_id) const override final
    {
        return query_graph.IsBackwardEdge(edge_based_node_id);
    }

    NodeID GetTarget(const EdgeID edge_based_edge_id) const override final
    {
        return query_graph.GetTarget(edge_based_edge_id);
    }

    const EdgeData &GetEdgeData(const EdgeID edge_based_edge_id) const override final
    {
        return query_graph.GetEdgeData(edge_based_edge_id);
    }

    EdgeRange GetBorderEdgeRange(const LevelID level,
                                 const NodeID edge_based_node_id) const override final
    {
        return query_graph.GetBorderEdgeRange(level, edge_based_node_id);
    }

    // searches for a specific edge
    EdgeID FindEdge(const NodeID edge_based_node_from,
                    const NodeID edge_based_node_to) const override final
    {
        return query_graph.FindEdge(edge_based_node_from, edge_based_node_to);
    }
};

template <>
class ContiguousInternalMemoryDataFacade<MLD> final
    : public ContiguousInternalMemoryDataFacadeBase,
      public ContiguousInternalMemoryAlgorithmDataFacade<MLD>
{
  private:
  public:
    ContiguousInternalMemoryDataFacade(const std::shared_ptr<ContiguousBlockAllocator> &allocator,
                                       const std::string &metric_name,
                                       const std::size_t exclude_index)
        : ContiguousInternalMemoryDataFacadeBase(allocator, metric_name, exclude_index),
          ContiguousInternalMemoryAlgorithmDataFacade<MLD>(allocator, metric_name, exclude_index)
    {
    }
};
} // namespace osrm::engine::datafacade

#endif // CONTIGUOUS_INTERNALMEM_DATAFACADE_HPP
