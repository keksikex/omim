#include "generator/road_access_generator.hpp"

#include "generator/osm_id.hpp"
#include "generator/routing_helpers.hpp"

#include "routing/road_access.hpp"
#include "routing/road_access_serialization.hpp"

#include "indexer/classificator.hpp"
#include "indexer/feature.hpp"
#include "indexer/feature_data.hpp"
#include "indexer/features_vector.hpp"

#include "coding/file_container.hpp"
#include "coding/file_writer.hpp"

#include "base/logging.hpp"
#include "base/string_utils.hpp"

#include "std/initializer_list.hpp"

#include "defines.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace routing;
using namespace std;

namespace
{
char constexpr kDelim[] = " \t\r\n";
string const kCar = "Car";
string const kPedestrian = "Pedestrian";
string const kBicycle = "Bicycle";

using TagMapping = routing::RoadAccessTagProcessor::TagMapping;

TagMapping const kCarTagMapping = {
    {OsmElement::Tag("access", "no"), RoadAccess::Type::No},
    {OsmElement::Tag("vehicle", "no"), RoadAccess::Type::No},
    {OsmElement::Tag("access", "private"), RoadAccess::Type::Private},
    {OsmElement::Tag("access", "destination"), RoadAccess::Type::Destination},
};

TagMapping const kPedestrianTagMapping = {
    {OsmElement::Tag("access", "no"), RoadAccess::Type::No},
    {OsmElement::Tag("foot", "no"), RoadAccess::Type::No},
};

TagMapping const kBicycleTagMapping = {
    {OsmElement::Tag("access", "no"), RoadAccess::Type::No},
    {OsmElement::Tag("bicycle", "no"), RoadAccess::Type::No},
};

VehicleMask ReadVehicleMaskFromString(string const & s)
{
  if (s == kCar)
    return kCarMask;
  if (s == kPedestrian)
    return kPedestrianMask;
  if (s == kBicycle)
    return kBicycleMask;
  LOG(LERROR, ("Could not read vehicle mask from the string", s));
  return kAllVehiclesMask;
}

string WriteVehicleMaskToString(VehicleMask vehicleMask)
{
  if (vehicleMask == kCarMask)
    return kCar;
  if (vehicleMask == kPedestrianMask)
    return kPedestrian;
  if (vehicleMask == kBicycleMask)
    return kBicycle;
  LOG(LERROR, ("Unsupported vehicle mask", vehicleMask));
  return "Unsupported VehicleMask";
}

bool ParseRoadAccess(string const & roadAccessPath, map<osm::Id, uint32_t> const & osmIdToFeatureId,
                     FeaturesVector const & featuresVector,
                     map<VehicleMask, RoadAccess> & roadAccessByMask)
{
  ifstream stream(roadAccessPath);
  if (!stream)
  {
    LOG(LWARNING, ("Could not open", roadAccessPath));
    return false;
  }

  vector<uint32_t> privateRoads;

  map<VehicleMask, map<Segment, RoadAccess::Type>> segmentType;

  auto addSegment = [&](Segment const & segment, VehicleMask vehicleMask,
                        RoadAccess::Type roadAccessType, uint64_t osmId) {
    auto & m = segmentType[vehicleMask];
    auto const emplaceRes = m.emplace(segment, roadAccessType);
    if (!emplaceRes.second)
      LOG(LWARNING, ("duplicate road access info for", osmId));
  };

  string line;
  for (uint32_t lineNo = 1;; ++lineNo)
  {
    if (!getline(stream, line))
      break;

    strings::SimpleTokenizer iter(line, kDelim);

    if (!iter)
    {
      LOG(LERROR, ("Error when parsing road access: empty line", lineNo));
      return false;
    }
    VehicleMask const vehicleMask = ReadVehicleMaskFromString(*iter);
    ++iter;

    if (!iter)
    {
      LOG(LERROR, ("Error when parsing road access: no road access type", lineNo));
      return false;
    }
    RoadAccess::Type roadAccessType;
    FromString(*iter, roadAccessType);
    ++iter;

    uint64_t osmId;
    if (!iter || !strings::to_uint64(*iter, osmId))
    {
      LOG(LERROR, ("Error when parsing road access: bad osm id at line", lineNo));
      return false;
    }
    ++iter;

    auto const it = osmIdToFeatureId.find(osm::Id::Way(osmId));
    // Even though this osm element has a tag that is interesting for us,
    // we have not created a feature from it. Possible reasons:
    // no primary tag, unsupported type, etc.
    if (it == osmIdToFeatureId.cend())
      continue;

    uint32_t const featureId = it->second;
    FeatureType ft;
    featuresVector.GetByIndex(featureId, ft);
    ft.ParseGeometry(FeatureType::BEST_GEOMETRY);

    // An area created from a way.
    if (ft.GetPointsCount() == 0)
      continue;

    uint32_t const numSegments = static_cast<uint32_t>(ft.GetPointsCount() - 1);

    // Set this road access type for the entire feature.
    for (uint32_t segmentIdx = 0; segmentIdx < numSegments; ++segmentIdx)
    {
      addSegment(Segment(kFakeNumMwmId, featureId, segmentIdx, true /* isForward */), vehicleMask,
                 roadAccessType, osmId);
      addSegment(Segment(kFakeNumMwmId, featureId, segmentIdx, false /* isForward */), vehicleMask,
                 roadAccessType, osmId);
    }
  }

  roadAccessByMask.clear();
  for (auto const vehicleMask : RoadAccess::GetSupportedVehicleMasks())
  {
    RoadAccess roadAccess;
    roadAccess.SetTypes(segmentType[vehicleMask]);
    roadAccessByMask.emplace(vehicleMask, roadAccess);
  }

  return true;
}
}  // namespace

namespace routing
{
// RoadAccessTagProcessor --------------------------------------------------------------------------
RoadAccessTagProcessor::RoadAccessTagProcessor(VehicleMask vehicleMask)
  : m_vehicleMask(vehicleMask), m_tagMapping(nullptr)
{
  if (vehicleMask == kCarMask)
    m_tagMapping = &kCarTagMapping;
  else if (vehicleMask == kPedestrianMask)
    m_tagMapping = &kPedestrianTagMapping;
  else if (vehicleMask == kBicycleMask)
    m_tagMapping = &kBicycleTagMapping;
  else
    CHECK(false, ("Road access info is not supported for this vehicle mask", vehicleMask));
}

void RoadAccessTagProcessor::Process(OsmElement const & elem, ofstream & oss) const
{
  // todo(@m) Add support for non-way elements, such as barrier=gate.
  if (elem.type != OsmElement::EntityType::Way)
    return;

  for (auto const & tag : elem.m_tags)
  {
    auto const it = m_tagMapping->find(tag);
    if (it == m_tagMapping->cend())
      continue;
    oss << WriteVehicleMaskToString(m_vehicleMask) << " " << ToString(it->second) << " " << elem.id
        << endl;
  }
}

// RoadAccessWriter ------------------------------------------------------------
RoadAccessWriter::RoadAccessWriter()
{
  for (auto const vehicleMask : RoadAccess::GetSupportedVehicleMasks())
    m_tagProcessors.emplace_back(vehicleMask);
}

void RoadAccessWriter::Open(string const & filePath)
{
  LOG(LINFO,
      ("Saving information about barriers and road access classes in osm id terms to", filePath));
  m_stream.open(filePath, ofstream::out);

  if (!IsOpened())
    LOG(LINFO, ("Cannot open file", filePath));
}

void RoadAccessWriter::Process(OsmElement const & elem)
{
  if (!IsOpened())
  {
    LOG(LWARNING, ("Tried to write to a closed barriers writer"));
    return;
  }

  for (auto const & p : m_tagProcessors)
    p.Process(elem, m_stream);
}

bool RoadAccessWriter::IsOpened() const { return m_stream && m_stream.is_open(); }

// RoadAccessCollector ----------------------------------------------------------
RoadAccessCollector::RoadAccessCollector(string const & dataFilePath, string const & roadAccessPath,
                                         string const & osmIdsToFeatureIdsPath)
{
  map<osm::Id, uint32_t> osmIdToFeatureId;
  if (!ParseOsmIdToFeatureIdMapping(osmIdsToFeatureIdsPath, osmIdToFeatureId))
  {
    LOG(LWARNING, ("An error happened while parsing feature id to osm ids mapping from file:",
                   osmIdsToFeatureIdsPath));
    m_valid = false;
    return;
  }

  FeaturesVectorTest featuresVector(dataFilePath);

  map<VehicleMask, RoadAccess> roadAccessByMask;
  if (!ParseRoadAccess(roadAccessPath, osmIdToFeatureId, featuresVector.GetVector(),
                       roadAccessByMask))
  {
    LOG(LWARNING, ("An error happened while parsing road access from file:", roadAccessPath));
    m_valid = false;
    return;
  }

  m_valid = true;
  m_roadAccessByMask.swap(roadAccessByMask);
}

// Functions ------------------------------------------------------------------
void BuildRoadAccessInfo(string const & dataFilePath, string const & roadAccessPath,
                         string const & osmIdsToFeatureIdsPath)
{
  LOG(LINFO, ("Generating road access info for", dataFilePath));

  RoadAccessCollector collector(dataFilePath, roadAccessPath, osmIdsToFeatureIdsPath);

  if (!collector.IsValid())
  {
    LOG(LWARNING, ("Unable to parse road access in osm terms"));
    return;
  }

  FilesContainerW cont(dataFilePath, FileWriter::OP_WRITE_EXISTING);
  FileWriter writer = cont.GetWriter(ROAD_ACCESS_FILE_TAG);

  RoadAccessSerializer::Serialize(writer, collector.GetRoadAccessByMask());
}
}  // namespace routing
