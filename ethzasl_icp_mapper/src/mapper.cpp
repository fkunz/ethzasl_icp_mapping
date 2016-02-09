#include <fstream>
#include <iostream>

#include <boost/version.hpp>
#include <boost/thread.hpp>
#if BOOST_VERSION >= 104100
	#include <boost/thread/future.hpp>
#endif // BOOST_VERSION >=  104100

#include "ros/ros.h"
#include "ros/console.h"
#include "pointmatcher/PointMatcher.h"
#include "pointmatcher/Timer.h"

#include "pointmatcher_ros/point_cloud.h"
#include "pointmatcher_ros/transform.h"
#include "pointmatcher_ros/get_params_from_server.h"
#include "pointmatcher_ros/ros_logger.h"

#include "nav_msgs/Odometry.h"
#include "tf/transform_broadcaster.h"
#include "tf_conversions/tf_eigen.h"
#include "tf/transform_listener.h"
#include "eigen_conversions/eigen_msg.h"

// Services
#include "std_msgs/String.h"
#include "std_msgs/Float64.h"
#include "std_srvs/Empty.h"
#include "map_msgs/GetPointMap.h"
#include "map_msgs/SaveMap.h"
#include "ethzasl_icp_mapper/LoadMap.h"
#include "ethzasl_icp_mapper/CorrectPose.h"
#include "ethzasl_icp_mapper/SetMode.h"
#include "ethzasl_icp_mapper/GetMode.h"
#include "ethzasl_icp_mapper/GetBoundedMap.h" // FIXME: should that be moved to map_msgs?

using namespace std;
using namespace PointMatcherSupport;

class Mapper
{
	typedef PointMatcher<float> PM;
	typedef PM::DataPoints DP;

	ros::NodeHandle& n;
	ros::NodeHandle& pn;
	
	// Subscribers
	ros::Subscriber scanSub;
	ros::Subscriber cloudSub;

	// Publisher
	ros::Publisher mapPub;
	ros::Publisher outlierPub;
	ros::Publisher odomPub;
	ros::Publisher odomErrorPub;
	ros::Publisher covariancePub;
	ros::Publisher meanErrorPub;
	ros::Publisher meanErrorScaledPub;
	ros::Publisher meanErrorScaledUnfilteredPub;
	ros::Publisher overlapPub;
	ros::Publisher overlapUnfilteredPub;
	ros::Publisher pointUsedRatioPub;
	ros::Publisher weightedPointUsedRatioPub;

	// Services
	ros::ServiceServer getPointMapSrv;
	ros::ServiceServer saveMapSrv;
	ros::ServiceServer loadMapSrv;
	ros::ServiceServer resetSrv;
	ros::ServiceServer correctPoseSrv;
	ros::ServiceServer setModeSrv;
	ros::ServiceServer getModeSrv;
	ros::ServiceServer getBoundedMapSrv;

	ros::Time mapCreationTime;
	ros::Time lastPoinCloudTime;

	PM::DataPointsFilters inputFilters;
	PM::DataPointsFilters mapPreFilters;
	PM::DataPointsFilters mapPostFilters;
	PM::DataPoints *mapPointCloud;
	PM::ICPSequence icp;
	unique_ptr<PM::Transformation> transformation;
	
	// multi-threading mapper
	#if BOOST_VERSION >= 104100
	typedef boost::packaged_task<PM::DataPoints*> MapBuildingTask;
	typedef boost::unique_future<PM::DataPoints*> MapBuildingFuture;
	boost::thread mapBuildingThread;
	MapBuildingTask mapBuildingTask;
	MapBuildingFuture mapBuildingFuture;
	bool mapBuildingInProgress;
	#endif // BOOST_VERSION >= 104100

	// Parameters
	bool useConstMotionModel;
	bool processingNewCloud;
	bool localizing;
	bool mapping;
	bool publishTfAsParent;
	int minReadingPointCount;
	int minMapPointCount;
	int inputQueueSize;
	double maxMeanError;
	double minOverlapToLocalize;
	double minOverlapToMerge;
	double maxMeanErrorToLocalize;
	double maxMeanErrorToMerge;
	double tfRefreshPeriod;  //!< if set to zero, tf will be publish at the rate of the incoming point cloud messages
	string odomFrame;
	string mapFrame;
	string vtkFinalMapName; //!< name of the final vtk map

	PM::TransformationParameters TOdomToMap;
	boost::thread publishThread;
	boost::mutex publishLock;
	ros::Time publishStamp;
	
	tf::TransformListener tfListener;
	tf::TransformBroadcaster tfBroadcaster;

	ofstream logfile;
	
public:
	Mapper(ros::NodeHandle& n, ros::NodeHandle& pn);
	~Mapper();

protected:
	void gotScan(const sensor_msgs::LaserScan& scanMsgIn);
	void gotCloud(const sensor_msgs::PointCloud2& cloudMsgIn);
	void processCloud(unique_ptr<DP> cloud, const std::string& scannerFrame, const ros::Time& stamp, uint32_t seq);
	bool prepareForICP(DP* cloud, const std::string& scannerFrame, const ros::Time& stamp, PM::TransformationParameters& TScannerToMap, PM::TransformationParameters& TOdomToScanner);
	bool applyICP(DP* cloud, const ros::Time& stamp, PM::TransformationParameters& TScannerToMap, PM::TransformationParameters& TOdomToScanner, PM::TransformationParameters& Ticp);
	void applyMapping(DP* cloud, const ros::Time& stamp, PM::TransformationParameters& Ticp);
	void writeDebugInfo(bool mapCurrentCloud, PM::TransformationParameters& TScannerToMap, PM::TransformationParameters& TOdomToScanner, PM::TransformationParameters& Ticp);
	void processNewMapIfAvailable();
	void setMap(DP* newPointCloud);
	DP* updateMap(DP* newPointCloud, const PM::TransformationParameters Ticp, bool updateExisting);
	void waitForMapBuildingCompleted();
	
	void publishLoop(double publishPeriod);
	void publishOdomTransform(const ros::Time& stamp = ros::Time::now());
	void publishTransform();
	
	// Services
	bool getPointMap(map_msgs::GetPointMap::Request &req, map_msgs::GetPointMap::Response &res);
	bool saveMap(map_msgs::SaveMap::Request &req, map_msgs::SaveMap::Response &res);
	bool loadMap(ethzasl_icp_mapper::LoadMap::Request &req, ethzasl_icp_mapper::LoadMap::Response &res);
	bool reset(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res);
	bool correctPose(ethzasl_icp_mapper::CorrectPose::Request &req, ethzasl_icp_mapper::CorrectPose::Response &res);
	bool setMode(ethzasl_icp_mapper::SetMode::Request &req, ethzasl_icp_mapper::SetMode::Response &res);
	bool getMode(ethzasl_icp_mapper::GetMode::Request &req, ethzasl_icp_mapper::GetMode::Response &res);
	bool getBoundedMap(ethzasl_icp_mapper::GetBoundedMap::Request &req, ethzasl_icp_mapper::GetBoundedMap::Response &res);
};

Mapper::Mapper(ros::NodeHandle& n, ros::NodeHandle& pn):
	n(n),
	pn(pn),
	mapPointCloud(0),
	transformation(PM::get().REG(Transformation).create("RigidTransformation")),
	#if BOOST_VERSION >= 104100
	mapBuildingInProgress(false),
	#endif // BOOST_VERSION >= 104100
	useConstMotionModel(getParam<bool>("useConstMotionModel", false)),
	processingNewCloud(false),
	localizing(getParam<bool>("localizing", true)),
	mapping(getParam<bool>("mapping", true)),
	minReadingPointCount(getParam<int>("minReadingPointCount", 2000)),
	minMapPointCount(getParam<int>("minMapPointCount", 500)),
	inputQueueSize(getParam<int>("inputQueueSize", 10)),
	maxMeanError(getParam<double>("maxMeanError", 0.8)),
	minOverlapToLocalize(getParam<double>("minOverlapToLocalize", 0.5)),
	minOverlapToMerge(getParam<double>("minOverlapToMerge", 0.3)),
	maxMeanErrorToLocalize(getParam<double>("maxMeanErrorToLocalize", 0.2)),
	maxMeanErrorToMerge(getParam<double>("maxMeanErrorToMerge", 0.2)),
	tfRefreshPeriod(getParam<double>("tfRefreshPeriod", 0.01)),
	publishTfAsParent(getParam<bool>("publish_tf_as_parent", true)),
	odomFrame(getParam<string>("odom_frame", "odom")),
	mapFrame(getParam<string>("map_frame", "map")),
	vtkFinalMapName(getParam<string>("vtkFinalMapName", "finalMap.vtk")),
	TOdomToMap(PM::TransformationParameters::Identity(4, 4)),
	publishStamp(ros::Time::now()),
	tfListener(ros::Duration(30))
{

	// Ensure proper states
	if(localizing == false)
		mapping = false;
	if(mapping == true)
		localizing = true;

	// set logger
	if (getParam<bool>("useROSLogger", false))
		PointMatcherSupport::setLogger(new PointMatcherSupport::ROSLogger);

	// load configs
	string configFileName;
	if (ros::param::get("~icpConfig", configFileName))
	{
		ifstream ifs(configFileName.c_str());
		if (ifs.good())
		{
			icp.loadFromYaml(ifs);
		}
		else
		{
			ROS_ERROR_STREAM("Cannot load ICP config from YAML file " << configFileName);
			icp.setDefault();
		}
	}
	else
	{
		ROS_INFO_STREAM("No ICP config file given, using default");
		icp.setDefault();
	}
	if (getParam<bool>("useROSLogger", false))
		PointMatcherSupport::setLogger(new PointMatcherSupport::ROSLogger);

	if (ros::param::get("~inputFiltersConfig", configFileName))
	{
		ifstream ifs(configFileName.c_str());
		if (ifs.good())
		{
			inputFilters = PM::DataPointsFilters(ifs);
		}
		else
		{
			ROS_ERROR_STREAM("Cannot load input filters config from YAML file " << configFileName);
		}
	}
	else
	{
		ROS_INFO_STREAM("No input filters config file given, not using these filters");
	}

	if (ros::param::get("~mapPreFiltersConfig", configFileName))
	{
		ifstream ifs(configFileName.c_str());
		if (ifs.good())
		{
			mapPreFilters = PM::DataPointsFilters(ifs);
		}
		else
		{
			ROS_ERROR_STREAM("Cannot load map pre-filters config from YAML file " << configFileName);
		}
	}
	else
	{
		ROS_INFO_STREAM("No map pre-filters config file given, not using these filters");
	}

	if (ros::param::get("~mapPostFiltersConfig", configFileName))
	{
		ifstream ifs(configFileName.c_str());
		if (ifs.good())
		{
			mapPostFilters = PM::DataPointsFilters(ifs);
		}
		else
		{
			ROS_ERROR_STREAM("Cannot load map post-filters config from YAML file " << configFileName);
		}
	}
	else
	{
		ROS_INFO_STREAM("No map post-filters config file given, not using these filters");
	}

	// topics and services initialization
	if (getParam<bool>("subscribe_scan", true))
		scanSub = n.subscribe("scan", inputQueueSize, &Mapper::gotScan, this);
	if (getParam<bool>("subscribe_cloud", true))
		cloudSub = n.subscribe("cloud_in", inputQueueSize, &Mapper::gotCloud, this);
	mapPub = n.advertise<sensor_msgs::PointCloud2>("point_map", 2, true);
	outlierPub = n.advertise<sensor_msgs::PointCloud2>("outliers", 2, true);
	odomPub = n.advertise<nav_msgs::Odometry>("icp_odom", 50, true);
	odomErrorPub = n.advertise<nav_msgs::Odometry>("icp_error_odom", 50, true);
	covariancePub = pn.advertise<std_msgs::Float64MultiArray>("covariance", 50, true);
	meanErrorPub = pn.advertise<std_msgs::Float64>("mean_error", 50, true);
	meanErrorScaledPub = pn.advertise<std_msgs::Float64>("mean_error_scaled", 50, true);
	meanErrorScaledUnfilteredPub = pn.advertise<std_msgs::Float64>("mean_error_scaled_unfiltered", 50, true);
	overlapPub = pn.advertise<std_msgs::Float64>("overlap", 50, true);
	overlapUnfilteredPub = pn.advertise<std_msgs::Float64>("overlap_unfiltered", 50, true);
	pointUsedRatioPub = pn.advertise<std_msgs::Float64>("pointUsedRatio", 50, true);
	weightedPointUsedRatioPub = pn.advertise<std_msgs::Float64>("weightedPointUsedRatio", 50, true);
	getPointMapSrv = n.advertiseService("dynamic_point_map", &Mapper::getPointMap, this);
	saveMapSrv = pn.advertiseService("save_map", &Mapper::saveMap, this);
	loadMapSrv = pn.advertiseService("load_map", &Mapper::loadMap, this);
	resetSrv = pn.advertiseService("reset", &Mapper::reset, this);
	correctPoseSrv = pn.advertiseService("correct_pose", &Mapper::correctPose, this);
	setModeSrv = pn.advertiseService("set_mode", &Mapper::setMode, this);
	getModeSrv = pn.advertiseService("get_mode", &Mapper::getMode, this);
	getBoundedMapSrv = pn.advertiseService("get_bounded_map", &Mapper::getBoundedMap, this);

	// refreshing tf transform thread
	publishThread = boost::thread(boost::bind(&Mapper::publishLoop, this, tfRefreshPeriod));

	logfile.open ("icp_mapper.log");
}

Mapper::~Mapper()
{
	#if BOOST_VERSION >= 104100
	// wait for map-building thread
	if (mapBuildingInProgress)
	{
		mapBuildingFuture.wait();
		if (mapBuildingFuture.has_value())
			delete mapBuildingFuture.get();
	}
	#endif // BOOST_VERSION >= 104100
	// wait for publish thread
	publishThread.join();
	// save point cloud
	if (mapPointCloud)
	{
		mapPointCloud->save(vtkFinalMapName);
		delete mapPointCloud;
	}
	logfile.close();
}

void Mapper::gotScan(const sensor_msgs::LaserScan& scanMsgIn)
{
	if(localizing)
	{
		const ros::Time endScanTime(scanMsgIn.header.stamp + ros::Duration(scanMsgIn.time_increment * (scanMsgIn.ranges.size() - 1)));
		unique_ptr<DP> cloud(new DP(PointMatcher_ros::rosMsgToPointMatcherCloud<float>(scanMsgIn, &tfListener, odomFrame)));
		processCloud(move(cloud), scanMsgIn.header.frame_id, endScanTime, scanMsgIn.header.seq);
	}
}

void Mapper::gotCloud(const sensor_msgs::PointCloud2& cloudMsgIn)
{
	if(localizing)
	{
		unique_ptr<DP> cloud(new DP(PointMatcher_ros::rosMsgToPointMatcherCloud<float>(cloudMsgIn)));
		processCloud(move(cloud), cloudMsgIn.header.frame_id, cloudMsgIn.header.stamp, cloudMsgIn.header.seq);
	}
}

struct BoolSetter
{
public:
	bool toSetValue;
	BoolSetter(bool& target, bool toSetValue):
		toSetValue(toSetValue),
		target(target)
	{}
	~BoolSetter()
	{
		target = toSetValue;
	}
protected:
	bool& target;
};

void Mapper::processCloud(unique_ptr<DP> newPointCloud, const std::string& scannerFrame, const ros::Time& stamp, uint32_t seq)
{
	processingNewCloud = true;
	BoolSetter stopProcessingSetter(processingNewCloud, false);

	// if the future has completed, use the new map
	processNewMapIfAvailable();
	
	// IMPORTANT:  We need to receive the point clouds in local coordinates (scanner or robot)
	timer t;

	DP* newDataPoints = newPointCloud.release();
	PM::TransformationParameters TScannerToMap, TOdomToScanner, Ticp;
	if (!prepareForICP(newDataPoints, scannerFrame, stamp, TScannerToMap, TOdomToScanner))
		return;

	bool mapCurrentCloud = applyICP(newDataPoints, stamp, TScannerToMap, TOdomToScanner, Ticp);

	writeDebugInfo(mapCurrentCloud, TScannerToMap, TOdomToScanner, Ticp);

	// check if news points should be added to the map
	if (mapCurrentCloud)
		ROS_WARN_STREAM("mapCurrentCloud");
	if (mapCurrentCloud &&
		#if BOOST_VERSION >= 104100
		(!mapBuildingInProgress)
		#else // BOOST_VERSION >= 104100
		true
		#endif // BOOST_VERSION >= 104100
	)
	{
		applyMapping(newDataPoints, stamp, Ticp);
	} else
	{
		delete newDataPoints;
	}

	//Statistics about time and real-time capability
	int realTimeRatio = 100*t.elapsed() / (stamp.toSec()-lastPoinCloudTime.toSec());
	ROS_INFO_STREAM("[TIME] Total ICP took: " << t.elapsed() << " [s]");
	if(realTimeRatio < 80)
		ROS_INFO_STREAM("[TIME] Real-time capability: " << realTimeRatio << "%");
	else
		ROS_WARN_STREAM("[TIME] Real-time capability: " << realTimeRatio << "%");

	lastPoinCloudTime = stamp;
}

bool Mapper::prepareForICP(DP* newPointCloud, const std::string& scannerFrame, const ros::Time& stamp,
						   PM::TransformationParameters& TScannerToMap,
						   PM::TransformationParameters& TOdomToScanner)
{
	// Convert point cloud
	const size_t goodCount(newPointCloud->features.cols());
	if (goodCount == 0)
	{
		ROS_ERROR("I found no good points in the cloud");
		return false;
	}

	// Dimension of the point cloud, important since we handle 2D and 3D
	const int dimp1(newPointCloud->features.rows());

	ROS_INFO("Processing new point cloud");
	{
		timer t; // Print how long take the algo

		// Apply filters to incoming cloud, in scanner coordinates
		inputFilters.apply(*newPointCloud);

		ROS_INFO_STREAM("Input filters took " << t.elapsed() << " [s]");
	}

	string reason;
	// Initialize the transformation to identity if empty
	if(!icp.hasMap())
	{
		// we need to know the dimensionality of the point cloud to initialize properly
		publishLock.lock();
		TOdomToMap = PM::TransformationParameters::Identity(dimp1, dimp1);
		publishLock.unlock();
	}

	// Fetch transformation from scanner to odom
	// Note: we don't need to wait for transform. It is already called in transformListenerToEigenMatrix()
	try
	{
		TOdomToScanner = PointMatcher_ros::eigenMatrixToDim<float>(
					PointMatcher_ros::transformListenerToEigenMatrix<float>(tfListener, scannerFrame, odomFrame, stamp),
					dimp1);
	}
	catch(tf::ExtrapolationException e)
	{
		ROS_ERROR_STREAM("Extrapolation Exception. stamp = "<< stamp << " now = " << ros::Time::now() << " delta = " << ros::Time::now() - stamp);
		return false;
	}

	ROS_DEBUG_STREAM("\n");
	ROS_DEBUG_STREAM("TOdomToScanner(" << odomFrame<< " to " << scannerFrame << "):\n" << TOdomToScanner);
	ROS_DEBUG_STREAM("TOdomToMap(" << odomFrame<< " to " << mapFrame << "):\n" << TOdomToMap);

	TScannerToMap = transformation->correctParameters(TOdomToMap * TOdomToScanner.inverse());
	ROS_DEBUG_STREAM("TscannerToMap (" << scannerFrame << " to " << mapFrame << "):\n" << TScannerToMap);

	// Ensure a minimum amount of point after filtering
	const int ptsCount = newPointCloud->features.cols();
	if(ptsCount < minReadingPointCount)
	{
		ROS_ERROR_STREAM("Not enough points in newPointCloud: only " << ptsCount << " pts.");
		return false;
	}

	// Initialize the map if empty
	if(!icp.hasMap())
	{
		ROS_INFO_STREAM("Creating an initial map");
		mapCreationTime = stamp;
		setMap(updateMap(newPointCloud, TScannerToMap, false));
		// we must not delete newPointCloud because we just stored it in the mapPointCloud
		return false;
	}

	// Check dimension
	if (newPointCloud->features.rows() != icp.getInternalMap().features.rows())
	{
		ROS_ERROR_STREAM("Dimensionality missmatch: incoming cloud is " << newPointCloud->features.rows()-1 << " while map is " << icp.getInternalMap().features.rows()-1);
		return false;
	}
	return true;
}

bool Mapper::applyICP(DP* newPointCloud, const ros::Time& stamp, PM::TransformationParameters& TScannerToMap, PM::TransformationParameters& TOdomToScanner, PM::TransformationParameters& Ticp)
{
	bool mapCurrentCloud = mapping;
	try
	{
		// Apply ICP
		Ticp = icp(*newPointCloud, TScannerToMap);

		ROS_DEBUG_STREAM("Ticp:\n" << Ticp);
		const double estimatedOverlap = icp.errorMinimizer->getOverlap();
		const double meanError = icp.errorMinimizer->getMean();
		ROS_DEBUG_STREAM("Overlap: " << estimatedOverlap);
		ROS_DEBUG_STREAM("Mean Error: " << meanError);

		// Ensure minimum overlap for localization
		if (estimatedOverlap < minOverlapToLocalize)
		{
			ROS_ERROR_STREAM("Estimated overlap too small, ignoring ICP correction!" << estimatedOverlap << "/" << minOverlapToLocalize);
			return false;
		}

		// Ensure maximum mean error for localization
		if (meanError > maxMeanErrorToLocalize)
		{
			ROS_ERROR_STREAM("Mean error too big, ignoring ICP correction! " << meanError << "/" << maxMeanErrorToLocalize);
			return false;
		}

		// Ensure maximum mean error for mapping
		if (meanError > maxMeanError)
		{
			ROS_ERROR_STREAM("Mean error too big, ignoring point cloud! " << meanError << "/" << maxMeanError);
			mapCurrentCloud = false;
		}

		// Ensure minimum scan quality for mapping
		if (estimatedOverlap > minOverlapToMerge &&
			meanError > maxMeanErrorToMerge)
		{
			ROS_ERROR_STREAM("Overlap to small and mean error to big, ignoring point cloud! "
							 << estimatedOverlap << "/" << minOverlapToMerge << " "
							 << meanError << "/" << maxMeanErrorToMerge);
			mapCurrentCloud = false;
		}

		// Compute tf
		publishStamp = stamp;
		publishLock.lock();
		TOdomToMap = Ticp * TOdomToScanner;
		// Publish tf
		publishOdomTransform(stamp);
		publishLock.unlock();
		processingNewCloud = false;

		// Publish odometry
		if (odomPub.getNumSubscribers())
			odomPub.publish(PointMatcher_ros::eigenMatrixToOdomMsg<float>(Ticp, mapFrame, stamp));

		// Publish error on odometry
		if (odomErrorPub.getNumSubscribers())
			odomErrorPub.publish(PointMatcher_ros::eigenMatrixToOdomMsg<float>(TOdomToMap, mapFrame, stamp));

		// Publish outliers
		if (outlierPub.getNumSubscribers())
		{
			//DP outliers = PM::extractOutliers(transformation->compute(*newPointCloud, Ticp), *mapPointCloud, 0.6);
			//outlierPub.publish(PointMatcher_ros::pointMatcherCloudToRosMsg<float>(outliers, mapFrame, mapCreationTime));
		}
	}
	catch (PM::ConvergenceError error)
	{
		ROS_ERROR_STREAM("ICP failed to converge: " << error.what());
		return false;
	}
	return mapCurrentCloud;
}

void Mapper::applyMapping(DP* newPointCloud, const ros::Time& stamp, PM::TransformationParameters& Ticp)
{
	// make sure we process the last available map
	mapCreationTime = stamp;
	#if BOOST_VERSION >= 104100
	ROS_INFO("Adding new points to the map in background");
	mapBuildingTask = MapBuildingTask(boost::bind(&Mapper::updateMap, this, newPointCloud, Ticp, true));
	mapBuildingFuture = mapBuildingTask.get_future();
	mapBuildingThread = boost::thread(boost::move(boost::ref(mapBuildingTask)));
	mapBuildingInProgress = true;
	#else // BOOST_VERSION >= 104100
	ROS_INFO("Adding new points to the map");
	setMap(updateMap( newPointCloud, Ticp, true));
	#endif // BOOST_VERSION >= 104100
}

void Mapper::writeDebugInfo(bool mapCurrentCloud, PM::TransformationParameters& TScannerToMap, PM::TransformationParameters& TOdomToScanner, PM::TransformationParameters& Ticp)
{
	if (covariancePub.getNumSubscribers())
	{
		std_msgs::Float64MultiArray covariance;
		tf::matrixEigenToMsg(icp.errorMinimizer->getCovariance(), covariance);
		covariancePub.publish(covariance);
	}

	std_msgs::Float64 float_msg;
	if (meanErrorPub.getNumSubscribers())
	{
		float_msg.data = icp.errorMinimizer->getMean();
		meanErrorPub.publish(float_msg);
	}

	if (meanErrorScaledUnfilteredPub.getNumSubscribers())
	{
		float_msg.data = icp.errorMinimizer->getMean() * 10;
		meanErrorScaledUnfilteredPub.publish(float_msg);
	}

	if (overlapUnfilteredPub.getNumSubscribers())
	{
		float_msg.data = icp.errorMinimizer->getOverlap();
		overlapUnfilteredPub.publish(float_msg);
	}

	if (pointUsedRatioPub.getNumSubscribers())
	{
		float_msg.data = icp.errorMinimizer->getPointUsedRatio();
		pointUsedRatioPub.publish(float_msg);
	}

	if (weightedPointUsedRatioPub.getNumSubscribers())
	{
		float_msg.data = icp.errorMinimizer->getWeightedPointUsedRatio();
		weightedPointUsedRatioPub.publish(float_msg);
	}

	if (overlapPub.getNumSubscribers() &&
		mapCurrentCloud)
	{
		float_msg.data = icp.errorMinimizer->getOverlap();
		overlapPub.publish(float_msg);
	}

	if (meanErrorScaledPub.getNumSubscribers() &&
		mapCurrentCloud)
	{
		float_msg.data = icp.errorMinimizer->getMean() * 10;
		meanErrorScaledPub.publish(float_msg);
	}

//	ROS_DEBUG_STREAM("Covariance:\n" << icp.errorMinimizer->getCovariance());
//	ROS_DEBUG_STREAM("Mean:\n" << icp.errorMinimizer->getMean());
//	ROS_DEBUG_STREAM("Overlap:\n" << icp.errorMinimizer->getOverlap());
//	ROS_DEBUG_STREAM("PointUsedRatio:\n" << icp.errorMinimizer->getPointUsedRatio());
//	ROS_DEBUG_STREAM("WeightedPointUsedRatio:\n" << icp.errorMinimizer->getWeightedPointUsedRatio());
//	ROS_DEBUG_STREAM("TscannerToMap:\n" << TscannerToMap);
//	ROS_DEBUG_STREAM("TOdomToScanner:\n" << TOdomToScanner);
//	ROS_DEBUG_STREAM("Ticp:\n" << Ticp);
	ROS_DEBUG_STREAM("TOdomToMap:\n" << Ticp * TOdomToScanner);

	logfile << "\nCovariance: " << icp.errorMinimizer->getCovariance();
	logfile << "\nMean: " << icp.errorMinimizer->getMean();
	logfile << "\nOverlap: " << icp.errorMinimizer->getOverlap();
	logfile << "\nPointUsedRatio: " << icp.errorMinimizer->getPointUsedRatio();
	logfile << "\nWeightedPointUsedRatio: " << icp.errorMinimizer->getWeightedPointUsedRatio();
	logfile << "\nTscannerToMap:\n" << TScannerToMap;
	logfile << "\nTOdomToScanner\n" << TOdomToScanner;
	logfile << "\nTicp\n" << Ticp;
	logfile << "\nTOdomToMap\n" << Ticp * TOdomToScanner;
	logfile << "\n-----------------------------------------------------------------------------------------------------\n";
}

void Mapper::processNewMapIfAvailable()
{
	#if BOOST_VERSION >= 104100
	if (mapBuildingInProgress && mapBuildingFuture.has_value())
	{
		ROS_INFO_STREAM("New map available");
		setMap(mapBuildingFuture.get());
		mapBuildingInProgress = false;
	}
	#endif // BOOST_VERSION >= 104100
}

void Mapper::setMap(DP* newPointCloud)
{
	// delete old map
	if (mapPointCloud)
		delete mapPointCloud;

	// set new map
	mapPointCloud = newPointCloud;
	icp.setMap(*mapPointCloud);

	// Publish map point cloud
	// FIXME this crash when used without descriptor
	if (mapPub.getNumSubscribers())
		mapPub.publish(PointMatcher_ros::pointMatcherCloudToRosMsg<float>(*mapPointCloud, mapFrame, mapCreationTime));
}

Mapper::DP* Mapper::updateMap(DP* newPointCloud, const PM::TransformationParameters Ticp, bool updateExisting)
{
	timer t;

	// Correct new points using ICP result
	*newPointCloud = transformation->compute(*newPointCloud, Ticp);
	
	// Preparation of cloud for inclusion in map
	mapPreFilters.apply(*newPointCloud);
	
	// Merge point clouds to map
	if (updateExisting)
		newPointCloud->concatenate(*mapPointCloud);

	// Map maintenance
	if(newPointCloud->features.cols() > minMapPointCount)
		mapPostFilters.apply(*newPointCloud);
	
	ROS_INFO_STREAM("[TIME] New map available (" << newPointCloud->features.cols() << " pts), update took " << t.elapsed() << " [s]");

	return newPointCloud;
}

void Mapper::waitForMapBuildingCompleted()
{
#if BOOST_VERSION >= 104100
	if (mapBuildingInProgress)
	{
		// we wait for now, in future we should kill it
		mapBuildingFuture.wait();
		mapBuildingInProgress = false;
	}
#endif // BOOST_VERSION >= 104100
}

void Mapper::publishLoop(double publishPeriod)
{
	if(publishPeriod == 0)
		return;
	ros::Rate r(1.0 / publishPeriod);
	while(ros::ok())
	{
		publishTransform();
		r.sleep();
	}
}

void Mapper::publishOdomTransform(const ros::Time& stamp)
{
	tf::StampedTransform odomStampedTransform;
	tf::Transform odomTransform(PointMatcher_ros::eigenMatrixToTransform<float>(TOdomToMap));
	if (publishTfAsParent)
		odomStampedTransform = tf::StampedTransform(odomTransform, stamp, mapFrame, odomFrame);
	else
		odomStampedTransform = tf::StampedTransform(odomTransform.inverse(), ros::Time::now(), odomFrame, mapFrame);
	tfBroadcaster.sendTransform(odomStampedTransform);
}

void Mapper::publishTransform()
{
	if(processingNewCloud == false)
	{
		publishLock.lock();
		// Note: we use now as timestamp to refresh the tf and avoid other buffer to be empty
		publishOdomTransform();
		publishLock.unlock();
	}
}

bool Mapper::getPointMap(map_msgs::GetPointMap::Request &req, map_msgs::GetPointMap::Response &res)
{
	if (!mapPointCloud)
		return false;

	// FIXME: do we need a mutex here?
	res.map = PointMatcher_ros::pointMatcherCloudToRosMsg<float>(*mapPointCloud, mapFrame, ros::Time::now());
	return true;
}

bool Mapper::saveMap(map_msgs::SaveMap::Request &req, map_msgs::SaveMap::Response &res)
{
	if (!mapPointCloud)
		return false;

	try
	{
		mapPointCloud->save(req.filename.data);
	}
	catch (const std::runtime_error& e)
	{
		ROS_ERROR_STREAM("Unable to save: " << e.what());
		return false;
	}

	ROS_INFO_STREAM("Map saved at " <<  req.filename.data << "with " << mapPointCloud->features.cols() << " points.");
	return true;
}

bool Mapper::loadMap(ethzasl_icp_mapper::LoadMap::Request &req, ethzasl_icp_mapper::LoadMap::Response &res)
{
	waitForMapBuildingCompleted();

	DP* cloud(new DP(DP::load(req.filename.data)));

	const int dim = cloud->features.rows();
	const int nbPts = cloud->features.cols();
	ROS_INFO_STREAM("Loading " << dim-1 << "D point cloud (" << req.filename.data << ") with " << nbPts << " points.");

	publishLock.lock();
	TOdomToMap = PM::TransformationParameters::Identity(dim,dim);
	publishLock.unlock();

	setMap(cloud);

	return true;
}

bool Mapper::reset(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res)
{
	waitForMapBuildingCompleted();

	// note: no need for locking as we do ros::spin(), to update if we go for multi-threading
	publishLock.lock();
	TOdomToMap = PM::TransformationParameters::Identity(4,4);
	publishLock.unlock();

	icp.clearMap();

	return true;
}

bool Mapper::correctPose(ethzasl_icp_mapper::CorrectPose::Request &req, ethzasl_icp_mapper::CorrectPose::Response &res)
{
	publishLock.lock();
	TOdomToMap = PointMatcher_ros::odomMsgToEigenMatrix<float>(req.odom);
	publishOdomTransform();
	publishLock.unlock();

	return true;
}

bool Mapper::setMode(ethzasl_icp_mapper::SetMode::Request &req, ethzasl_icp_mapper::SetMode::Response &res)
{
	// Impossible states
	if(req.localize == false && req.map == true)
		return false;

	localizing = req.localize;
	mapping = req.map;
	
	return true;
}

bool Mapper::getMode(ethzasl_icp_mapper::GetMode::Request &req, ethzasl_icp_mapper::GetMode::Response &res)
{
	res.localize = localizing;
	res.map = mapping;
	return true;
}

bool Mapper::getBoundedMap(ethzasl_icp_mapper::GetBoundedMap::Request &req, ethzasl_icp_mapper::GetBoundedMap::Response &res)
{
	if (!mapPointCloud)
		return false;

	const float max_x = req.topRightCorner.x;
	const float max_y = req.topRightCorner.y;
	const float max_z = req.topRightCorner.z;

	const float min_x = req.bottomLeftCorner.x;
	const float min_y = req.bottomLeftCorner.y;
	const float min_z = req.bottomLeftCorner.z;

	cerr << "min [" << min_x << ", " << min_y << ", " << min_z << "] " << endl;
	cerr << "max [" << max_x << ", " << max_y << ", " << max_z << "] " << endl;

	tf::StampedTransform stampedTr;

	Eigen::Affine3d eigenTr;
	tf::poseMsgToEigen(req.mapCenter, eigenTr);
	Eigen::MatrixXf T = eigenTr.matrix().inverse().cast<float>();
	//const Eigen::MatrixXf T = eigenTr.matrix().cast<float>();

	cerr << "T:" << endl << T << endl;
	T = transformation->correctParameters(T);

	// FIXME: do we need a mutex here?
	const DP centeredPointCloud = transformation->compute(*mapPointCloud, T);
	DP cutPointCloud = centeredPointCloud.createSimilarEmpty();

	cerr << centeredPointCloud.features.topLeftCorner(3, 10) << endl;
	cerr << T << endl;

	int newPtCount = 0;
	for(int i=0; i < centeredPointCloud.features.cols(); i++)
	{
		const float x = centeredPointCloud.features(0,i);
		const float y = centeredPointCloud.features(1,i);
		const float z = centeredPointCloud.features(2,i);
		
		if(x < max_x && x > min_x &&
		   y < max_y && y > min_y &&
		   z < max_z && z > min_z)
		{
			cutPointCloud.setColFrom(newPtCount, centeredPointCloud, i);
			newPtCount++;
		}
	}

	cerr << "Extract " << newPtCount << " points from the map" << endl;

	cutPointCloud.conservativeResize(newPtCount);
	cutPointCloud = transformation->compute(cutPointCloud, T.inverse());

	// Send the resulting point cloud in ROS format
	res.boundedMap = PointMatcher_ros::pointMatcherCloudToRosMsg<float>(cutPointCloud, mapFrame, ros::Time::now());
	return true;
}

// Main function supporting the Mapper class
int main(int argc, char **argv)
{
	ros::init(argc, argv, "mapper");
	ros::NodeHandle n;
	ros::NodeHandle pn("~");
	Mapper mapper(n, pn);
	ros::spin();
	ros::shutdown();

	return 0;
}
