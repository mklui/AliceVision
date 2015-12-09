/* 
 * File:   main_rigLocalizer.cpp
 * Author: sgaspari
 *
 * Created on November 23, 2015, 6:48 PM
 */

#include <openMVG/localization/VoctreeLocalizer.hpp>
#if HAVE_CCTAG
#include <openMVG/localization/CCTagLocalizer.hpp>
#endif
#include <openMVG/rig/Rig.hpp>
#include <openMVG/sfm/pipelines/localization/SfM_Localizer.hpp>
#include <openMVG/image/image_io.hpp>
#include <openMVG/dataio/FeedProvider.hpp>
#include <openMVG/logger.hpp>

#include <boost/filesystem.hpp>
#include <boost/progress.hpp>
#include <boost/program_options.hpp> 
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/sum.hpp>

#include <iostream>
#include <string>
#include <chrono>

#if HAVE_ALEMBIC
#include <openMVG/dataio/AlembicExporter.hpp>
#endif // HAVE_ALEMBIC


namespace bfs = boost::filesystem;
namespace bacc = boost::accumulators;
namespace po = boost::program_options;

using namespace openMVG;

enum DescriberType
{
  SIFT
#if HAVE_CCTAG
  ,CCTAG,
  SIFT_CCTAG
#endif
};

inline DescriberType stringToDescriberType(const std::string& describerType)
{
  if(describerType == "SIFT")
    return DescriberType::SIFT;
#if HAVE_CCTAG
  if (describerType == "CCTAG")
    return DescriberType::CCTAG;
  if(describerType == "SIFT_CCTAG")
    return DescriberType::SIFT_CCTAG;
#endif
  throw std::invalid_argument("Unsupported describer type "+describerType);
}

inline std::string describerTypeToString(DescriberType describerType)
{
  if(describerType == DescriberType::SIFT)
    return "SIFT";
#if HAVE_CCTAG
  if (describerType == DescriberType::CCTAG)
    return "CCTAG";
  if(describerType == DescriberType::SIFT_CCTAG)
    return "SIFT_CCTAG";
#endif
  throw std::invalid_argument("Unrecognized DescriberType "+std::to_string(describerType));
}

std::ostream& operator<<(std::ostream& os, const DescriberType describerType)
{
  os << describerTypeToString(describerType);
  return os;
}

std::istream& operator>>(std::istream &in, DescriberType &describerType)
{
  int i;
  in >> i;
  describerType = static_cast<DescriberType>(i);
  return in;
}

std::string myToString(std::size_t i, std::size_t zeroPadding)
{
  std::stringstream ss;
  ss << std::setw(zeroPadding) << std::setfill('0') << i;
  return ss.str();
}

int main(int argc, char** argv)
{
  // common parameters
  std::string sfmFilePath;                //< the OpenMVG .json data file
  std::string descriptorsFolder;          //< the OpenMVG .json data file
  std::string mediaPath;                  //< the media file to localize
  std::string filelist;                  //< the media file to localize
  std::string rigCalibPath;               //< the file containing the calibration data for the file (subposes)
  std::string preset = features::describerPreset_enumToString(features::EDESCRIBER_PRESET::NORMAL_PRESET);               //< the preset for the feature extractor
  std::string str_descriptorType = describerTypeToString(DescriberType::SIFT);               //< the preset for the feature extractor
  bool refineIntrinsics = false;
  // parameters for voctree localizer
  std::string vocTreeFilepath;            //< the vocabulary tree file
  std::string weightsFilepath;            //< the vocabulary tree weights file
  std::string algostring = "FirstBest";   //< the localization algorithm to use for the voctree localizer
  size_t numResults = 10;                 //< number of documents to search when querying the voctree
  // parameters for cctag localizer
  size_t nNearestKeyFrames = 5;           //

#if HAVE_ALEMBIC
  std::string exportFile = "trackedcameras.abc"; //!< the export file
#endif
  
  std::size_t numCameras = 3;
  po::options_description desc("This program is used to localize a camera rig composed of internally calibrated cameras");
  desc.add_options()
          ("help,h", "Print this message")
          ("sfmdata,d", po::value<std::string>(&sfmFilePath)->required(), "The sfm_data.json kind of file generated by OpenMVG [it could be also a bundle.out to use an older version of OpenMVG]")
          ("siftPath,s", po::value<std::string>(&descriptorsFolder), "Folder containing the .desc. If not provided, it will be assumed to be parent(sfmdata)/matches [for the older version of openMVG it is the list.txt]")
          ("mediapath,m", po::value<std::string>(&mediaPath)->required(), "The folder path containing all the synchronised image subfolders assocated to each camera")
          ("filelist", po::value<std::string>(&filelist), "An optional txt file containing the images to use for calibration. This file must have the same name in each camera folder and contains the list of images to load.")
          ("refineIntrinsics,", po::bool_switch(&refineIntrinsics), "Enable/Disable camera intrinsics refinement for each localized image")
          ("nCameras", po::value<size_t>(&numCameras)->default_value(numCameras), "Number of cameras composing the rig")
          ("preset,", po::value<std::string>(&preset)->default_value(preset), "Preset for the feature extractor when localizing a new image {LOW,NORMAL,HIGH,ULTRA}")
          ("descriptors,", po::value<std::string>(&str_descriptorType)->default_value(str_descriptorType), "Type of descriptors to use")
          ("calibration,c", po::value<std::string>(&rigCalibPath)->required(), "The file containing the calibration data for the file (subposes)")
  // parameters for voctree localizer
          ("voctree,t", po::value<std::string>(&vocTreeFilepath), "Filename for the vocabulary tree")
          ("weights,w", po::value<std::string>(&weightsFilepath), "Filename for the vocabulary tree weights")
          ("algorithm,", po::value<std::string>(&algostring)->default_value(algostring), "Algorithm type: FirstBest=0, BestResult=1, AllResults=2, Cluster=3" )
          ("results,r", po::value<size_t>(&numResults)->default_value(numResults), "Number of images to retrieve in database")
#if HAVE_CCTAG
  // parameters for cctag localizer
          ("nNearestKeyFrames", po::value<size_t>(&nNearestKeyFrames)->default_value(nNearestKeyFrames), "Number of images to retrieve in database")
#endif
#if HAVE_ALEMBIC
          ("export,e", po::value<std::string>(&exportFile)->default_value(exportFile), "Filename for the SfM_Data export file (where camera poses will be stored). Default : trackedcameras.json If Alambic is enable it will also export an .abc file of the scene with the same name")
#endif
          ;

  po::variables_map vm;

  try
  {
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if(vm.count("help") || (argc == 1))
    {
      POPART_COUT(desc);
      return EXIT_SUCCESS;
    }

    po::notify(vm);
  }
  catch(boost::program_options::required_option& e)
  {
    POPART_CERR("ERROR: " << e.what() << std::endl);
    POPART_COUT("Usage:\n\n" << desc);
    return EXIT_FAILURE;
  }
  catch(boost::program_options::error& e)
  {
    POPART_CERR("ERROR: " << e.what() << std::endl);
    POPART_COUT("Usage:\n\n" << desc);
    return EXIT_FAILURE;
  }
  // just debugging prints
  {
    POPART_COUT("Program called with the following parameters:");
    POPART_COUT("\tsfmdata: " << sfmFilePath);
    POPART_COUT("\tmediapath: " << mediaPath);
    POPART_COUT("\tsiftPath: " << descriptorsFolder);
    POPART_COUT("\trefineIntrinsics: " << refineIntrinsics);
    POPART_COUT("\tnCameras: " << numCameras);
    POPART_COUT("\tpreset: " << preset);
    if(!filelist.empty())
      POPART_COUT("\tfilelist: " << filelist);
    POPART_COUT("\tdescriptors: " << str_descriptorType);
    if((DescriberType::SIFT==stringToDescriberType(str_descriptorType))
#if HAVE_CCTAG
            ||(DescriberType::SIFT_CCTAG==stringToDescriberType(str_descriptorType))
#endif
      )
    {
      // parameters for voctree localizer
      POPART_COUT("\tvoctree: " << vocTreeFilepath);
      POPART_COUT("\tweights: " << weightsFilepath);
      POPART_COUT("\talgorithm: " << algostring);
      POPART_COUT("\tresults: " << numResults);
    }
#if HAVE_CCTAG
    else
    {
      POPART_COUT("\tnNearestKeyFrames: " << nNearestKeyFrames);
    }
#endif

  }

  localization::LocalizerParameters *param;
  
  localization::ILocalizer *localizer;
  
  DescriberType describer = stringToDescriberType(str_descriptorType);
  
  // initialize the localizer according to the chosen type of describer
  if((DescriberType::SIFT==describer)
#if HAVE_CCTAG
            ||(DescriberType::SIFT_CCTAG==describer)
#endif
      )
  {
    localizer = new localization::VoctreeLocalizer(sfmFilePath,
                                           descriptorsFolder,
                                           vocTreeFilepath,
                                           weightsFilepath
#if HAVE_CCTAG
                                           , DescriberType::SIFT_CCTAG==describer
#endif
                                           );
    param = new localization::VoctreeLocalizer::Parameters();
    param->_featurePreset = features::describerPreset_stringToEnum(preset);
    param->_refineIntrinsics = refineIntrinsics;
    localization::VoctreeLocalizer::Parameters *casted = static_cast<localization::VoctreeLocalizer::Parameters *>(param);
    casted->_algorithm = localization::VoctreeLocalizer::initFromString(algostring);;
    casted->_numResults = numResults;
  }
#if HAVE_CCTAG
  else
  {
    localizer = new localization::CCTagLocalizer(sfmFilePath, descriptorsFolder);
    param = new localization::CCTagLocalizer::Parameters();
    param->_featurePreset = features::describerPreset_stringToEnum(preset);
    param->_refineIntrinsics = refineIntrinsics;
    localization::CCTagLocalizer::Parameters *casted = static_cast<localization::CCTagLocalizer::Parameters *>(param);
    casted->_nNearestKeyFrames = nNearestKeyFrames;
  }
#endif 


  if(!localizer->isInit())
  {
    POPART_CERR("ERROR while initializing the localizer!");
    return EXIT_FAILURE;
  }

#if HAVE_ALEMBIC
  dataio::AlembicExporter exporter(exportFile);
  exporter.addPoints(localizer->getSfMData().GetLandmarks());
#endif

  vector<dataio::FeedProvider*> feeders(numCameras);

  // Init the feeder for each camera
  for(int idCamera = 0; idCamera < numCameras; ++idCamera)
  {
    const std::string subMediaFilepath = mediaPath + "/" + std::to_string(idCamera);
    const std::string calibFile = subMediaFilepath + "/intrinsics.txt";
    const std::string feedPath = subMediaFilepath + "/"+filelist;

    // create the feedProvider
    feeders[idCamera] = new dataio::FeedProvider(feedPath, calibFile);
    if(!feeders[idCamera]->isInit())
    {
      POPART_CERR("ERROR while initializing the FeedProvider for the camera " 
              << idCamera << " " << feedPath);
      return EXIT_FAILURE;
    }
  }
  
  bool haveNext = true;
  size_t frameCounter = 0;
  
  // load the subposes
  std::vector<geometry::Pose3> vec_subPoses;
  rig::loadRigCalibration(rigCalibPath, vec_subPoses);
  assert(vec_subPoses.size() == numCameras-1);
  geometry::Pose3 rigPose;
  
  // Define an accumulator set for computing the mean and the
  // standard deviation of the time taken for localization
  bacc::accumulator_set<double, bacc::stats<bacc::tag::mean, bacc::tag::min, bacc::tag::max, bacc::tag::sum > > stats;

  
  while(haveNext)
  {
    // @fixme It's better to have arrays of pointers...
    std::vector<image::Image<unsigned char> > vec_imageGrey;
    std::vector<cameras::Pinhole_Intrinsic_Radial_K3 > vec_queryIntrinsics;
    vec_imageGrey.reserve(numCameras);
    vec_queryIntrinsics.reserve(numCameras);
           
    // for each camera get the image and the associated internal parameters
    for(int idCamera = 0; idCamera < numCameras; ++idCamera)
    {
      image::Image<unsigned char> imageGrey;
      cameras::Pinhole_Intrinsic_Radial_K3 queryIntrinsics;
      bool hasIntrinsics = false;
      std::string currentImgName;
      haveNext = feeders[idCamera]->next(imageGrey, queryIntrinsics, currentImgName, hasIntrinsics);
      
      if(!haveNext)
      {
        if(idCamera > 0)
        {
          // this is quite odd, it means that eg the fist camera has an image but
          // one of the others has not image
          POPART_CERR("This is weird... Camera " << idCamera << " seems not to have any available images while some other cameras do...");
          return EXIT_FAILURE;  // a bit harsh but if we are here it's cheesy to say the less
        }
        break;
      }
      
      // for now let's suppose that the cameras are calibrated internally too
      if(!hasIntrinsics)
      {
        POPART_CERR("For now only internally calibrated cameras are supported!"
                << "\nCamera " << idCamera << " does not have calibration for image " << currentImgName);
        return EXIT_FAILURE;  // a bit harsh but if we are here it's cheesy to say the less
      }
      
      vec_imageGrey.push_back(imageGrey);
      vec_queryIntrinsics.push_back(queryIntrinsics);
    }
    
    if(!haveNext)
    {
      // no more images are available
      break;
    }
    
    POPART_COUT("******************************");
    POPART_COUT("FRAME " << myToString(frameCounter, 4));
    POPART_COUT("******************************");
    auto detect_start = std::chrono::steady_clock::now();
    const bool isLocalized = localizer->localizeRig(vec_imageGrey,
                                                    param,
                                                    vec_queryIntrinsics,
                                                    vec_subPoses,
                                                    rigPose);
    auto detect_end = std::chrono::steady_clock::now();
    auto detect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(detect_end - detect_start);
    POPART_COUT("\nLocalization took  " << detect_elapsed.count() << " [ms]");
    stats(detect_elapsed.count());
    
    //@todo do something with the pose
    
#if HAVE_ALEMBIC
    if(isLocalized)
    {
      // for now just save the position of the main camera
      exporter.appendCamera("camera." + myToString(frameCounter, 4), rigPose, &vec_queryIntrinsics[0], mediaPath, frameCounter, frameCounter);
    }
    else
    {
      exporter.appendCamera("camera.V." + myToString(frameCounter, 4), geometry::Pose3(), &vec_queryIntrinsics[0], mediaPath, frameCounter, frameCounter);
    }
#endif

    ++frameCounter;
    
    
  }
  
  // print out some time stats
  POPART_COUT("\n\n******************************");
  POPART_COUT("Localized " << frameCounter << " images");
  POPART_COUT("Processing took " << bacc::sum(stats) / 1000 << " [s] overall");
  POPART_COUT("Mean time for localization:   " << bacc::mean(stats) << " [ms]");
  POPART_COUT("Max time for localization:   " << bacc::max(stats) << " [ms]");
  POPART_COUT("Min time for localization:   " << bacc::min(stats) << " [ms]");
}
