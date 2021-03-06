
#include <iostream>
#include <algorithm>
#include <numeric>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "camFusion.hpp"
#include "dataStructures.h"

using namespace std;


// Create groups of Lidar points whose projection into the camera falls into the same bounding box
void clusterLidarWithROI(std::vector<BoundingBox> &boundingBoxes, std::vector<LidarPoint> &lidarPoints, float shrinkFactor, cv::Mat &P_rect_xx, cv::Mat &R_rect_xx, cv::Mat &RT)
{
    // loop over all Lidar points and associate them to a 2D bounding box
    cv::Mat X(4, 1, cv::DataType<double>::type);
    cv::Mat Y(3, 1, cv::DataType<double>::type);

    for (auto it1 = lidarPoints.begin(); it1 != lidarPoints.end(); ++it1)
    {
        // assemble vector for matrix-vector-multiplication
        X.at<double>(0, 0) = it1->x;
        X.at<double>(1, 0) = it1->y;
        X.at<double>(2, 0) = it1->z;
        X.at<double>(3, 0) = 1;

        // project Lidar point into camera
        Y = P_rect_xx * R_rect_xx * RT * X;
        cv::Point pt;
        // pixel coordinates
        pt.x = Y.at<double>(0, 0) / Y.at<double>(2, 0); 
        pt.y = Y.at<double>(1, 0) / Y.at<double>(2, 0); 

        vector<vector<BoundingBox>::iterator> enclosingBoxes; // pointers to all bounding boxes which enclose the current Lidar point
        for (vector<BoundingBox>::iterator it2 = boundingBoxes.begin(); it2 != boundingBoxes.end(); ++it2)
        {
            // shrink current bounding box slightly to avoid having too many outlier points around the edges
            cv::Rect smallerBox;
            smallerBox.x = (*it2).roi.x + shrinkFactor * (*it2).roi.width / 2.0;
            smallerBox.y = (*it2).roi.y + shrinkFactor * (*it2).roi.height / 2.0;
            smallerBox.width = (*it2).roi.width * (1 - shrinkFactor);
            smallerBox.height = (*it2).roi.height * (1 - shrinkFactor);

            // check wether point is within current bounding box
            if (smallerBox.contains(pt))
            {
                enclosingBoxes.push_back(it2);
            }

        } // eof loop over all bounding boxes

        // check wether point has been enclosed by one or by multiple boxes
        if (enclosingBoxes.size() == 1)
        { 
            // add Lidar point to bounding box
            enclosingBoxes[0]->lidarPoints.push_back(*it1);
        }

    } // eof loop over all Lidar points
}

/* 
* The show3DObjects() function below can handle different output image sizes, but the text output has been manually tuned to fit the 2000x2000 size. 
* However, you can make this function work for other sizes too.
* For instance, to use a 1000x1000 size, adjusting the text positions by dividing them by 2.
*/
void show3DObjects(std::vector<BoundingBox> &boundingBoxes, cv::Size worldSize, cv::Size imageSize, bool bWait)
{
    // create topview image
    cv::Mat topviewImg(imageSize, CV_8UC3, cv::Scalar(255, 255, 255));

    for(auto it1=boundingBoxes.begin(); it1!=boundingBoxes.end(); ++it1)
    {
        // create randomized color for current 3D object
        cv::RNG rng(it1->boxID);
        cv::Scalar currColor = cv::Scalar(rng.uniform(0,150), rng.uniform(0, 150), rng.uniform(0, 150));

        // plot Lidar points into top view image
        int top=1e8, left=1e8, bottom=0.0, right=0.0; 
        float xwmin=1e8, ywmin=1e8, ywmax=-1e8;
        for (auto it2 = it1->lidarPoints.begin(); it2 != it1->lidarPoints.end(); ++it2)
        {
            // world coordinates
            float xw = (*it2).x; // world position in m with x facing forward from sensor
            float yw = (*it2).y; // world position in m with y facing left from sensor
            xwmin = xwmin<xw ? xwmin : xw;
            ywmin = ywmin<yw ? ywmin : yw;
            ywmax = ywmax>yw ? ywmax : yw;

            // top-view coordinates
            int y = (-xw * imageSize.height / worldSize.height) + imageSize.height;
            int x = (-yw * imageSize.width / worldSize.width) + imageSize.width / 2;

            // find enclosing rectangle
            top = top<y ? top : y;
            left = left<x ? left : x;
            bottom = bottom>y ? bottom : y;
            right = right>x ? right : x;

            // draw individual point
            cv::circle(topviewImg, cv::Point(x, y), 4, currColor, -1);
        }

        // draw enclosing rectangle
        cv::rectangle(topviewImg, cv::Point(left, top), cv::Point(right, bottom),cv::Scalar(0,0,0), 2);

        // augment object with some key data
        char str1[200], str2[200];
        sprintf(str1, "id=%d, #pts=%d", it1->boxID, (int)it1->lidarPoints.size());
        putText(topviewImg, str1, cv::Point2f(left-250, bottom+50), cv::FONT_ITALIC, 2, currColor);
        sprintf(str2, "xmin=%2.2f m, yw=%2.2f m", xwmin, ywmax-ywmin);
        putText(topviewImg, str2, cv::Point2f(left-250, bottom+125), cv::FONT_ITALIC, 2, currColor);  
    }

    // plot distance markers
    float lineSpacing = 2.0; // gap between distance markers
    int nMarkers = floor(worldSize.height / lineSpacing);
    for (size_t i = 0; i < nMarkers; ++i)
    {
        int y = (-(i * lineSpacing) * imageSize.height / worldSize.height) + imageSize.height;
        cv::line(topviewImg, cv::Point(0, y), cv::Point(imageSize.width, y), cv::Scalar(255, 0, 0));
    }

    // display image
    string windowName = "3D Objects";
    cv::namedWindow(windowName, 1);
    cv::imshow(windowName, topviewImg);

    if(bWait)
    {
        cv::waitKey(0); // wait for key to be pressed
    }
}


// associate a given bounding box with the keypoints it contains
void clusterKptMatchesWithROI(BoundingBox &boundingBox, std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, std::vector<cv::DMatch> &kptMatches)
{
    /*
        Before a TTC estimate can be computed in the next exercise, you need to find all keypoint matches that belong to each 3D object. 
        You can do this by simply checking whether the corresponding keypoints are within the region of interest in the camera image. All 
        matches which satisfy this condition should be added to a vector. The problem you will find is that there will be outliers among your
        matches. To eliminate those, I recommend that you compute a robust mean of all the euclidean distances between keypoint matches and 
        then remove those that are too far away from the mean.

        The task is complete once the code performs as described and adds the keypoint correspondences to the "kptMatches" property of the respective
        bounding boxes. Also, outlier matches have been removed based on the euclidean distance between them in relation to all the matches in the bounding box.
    */

    vector<double> distancias;
    for (cv::DMatch m : kptMatches) 
    {
    	cv::KeyPoint prevPoints = kptsPrev[m.queryIdx];
    	cv::KeyPoint currPoints = kptsCurr[m.trainIdx];
    	distancias.push_back(sqrt(pow(prevPoints.pt.x - currPoints.pt.x, 2) + pow(prevPoints.pt.y - currPoints.pt.y, 2)));
    }
	double meanDistance = accumulate(distancias.begin(), distancias.end(), 0.0);
    meanDistance /= distancias.size();
    double standarDev = 0.0;
    for (double d : distancias) 
    {
    	standarDev += pow(d - meanDistance, 2);
    }
    standarDev = sqrt(standarDev / distancias.size());

    for (int i = 0; i < kptMatches.size(); i++) 
    {
    	if (abs(distancias[i] - meanDistance) < standarDev) 
        {
    		boundingBox.kptMatches.push_back(kptMatches[i]);
    	}
    }
}


// Compute time-to-collision (TTC) based on keypoint correspondences in successive images
void computeTTCCamera(std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, 
                      std::vector<cv::DMatch> kptMatches, double frameRate, double &TTC, cv::Mat *visImg)
{
    /*
        Once keypoint matches have been added to the bounding boxes, the next step is to compute the TTC 
        estimate. As with Lidar, we already looked into this in the second lesson of this course, so you 
        please revisit the respective section and use the code sample there as a starting point for this 
        task here. Once you have your estimate of the TTC, please return it to the main function at the 
        end of computeTTCCamera.

        The task is complete once the code is functional and returns the specified output. Also, the code must be 
        able to deal with outlier correspondences in a statistically robust way to avoid severe estimation errors.
    */

    vector<double> distRatios;
    for (auto it1 = kptMatches.begin(); it1 != kptMatches.end(); it1++) 
    {
    	cv::KeyPoint prevOutlier = kptsPrev[it1->queryIdx];
    	cv::KeyPoint currOutlier = kptsCurr[it1->trainIdx];
    	for (auto it2 = kptMatches.begin() + 1; it2 != kptMatches.end(); it2++) 
        {
    		cv::KeyPoint prevInlier = kptsPrev[it2->queryIdx];
    		cv::KeyPoint currInlier = kptsCurr[it2->trainIdx];
    		double currentDistance = cv::norm(currOutlier.pt - currInlier.pt);
    		double prevDistance = cv::norm(prevOutlier.pt - prevInlier.pt);
    		if (prevDistance > std::numeric_limits<double>::epsilon() && currentDistance > 90) 
            {
    			distRatios.push_back(currentDistance / prevDistance);
    		}
    	}
    }
    if (distRatios.size() == 0) {
    	TTC = NAN;
    	return;
    }

    double dt = 1 / frameRate;
    int size = distRatios.size();
    std::sort(distRatios.begin(), distRatios.end());
    double medianDistRatio = 0.0;
    if (size % 2 == 0) 
    {
        medianDistRatio = (distRatios[size / 2] + distRatios[size/2 - 1]) / 2;
    } 
    else 
    {
        medianDistRatio = distRatios[size / 2];
    }

    TTC = -dt / (1 - medianDistRatio);
}


void computeTTCLidar(std::vector<LidarPoint> &lidarPointsPrev,
                     std::vector<LidarPoint> &lidarPointsCurr, double frameRate, double &TTC)
{
    /*
        In this part of the final project, your task is to compute the time-to-collision for all 
        matched 3D objects based on Lidar measurements alone. Please take a look at the "Lesson 3: 
        Engineering a Collision Detection System" of this course to revisit the theory behind TTC 
        estimation. Also, please implement the estimation in a way that makes it robust against 
        outliers which might be way too close and thus lead to faulty estimates of the TTC. Please 
        return your TCC to the main function at the end of computeTTCLidar.

        The task is complete once the code is functional and returns the specified output. Also, the 
        code is able to deal with outlier Lidar points in a statistically robust way to avoid severe 
        estimation errors.
    */

    vector<double> dPrev, dCurr;

    for (LidarPoint p : lidarPointsPrev)
    {
    	dPrev.push_back(p.x);
    }
    for (LidarPoint p : lidarPointsCurr) 
    {
    	dCurr.push_back(p.x);
    }

    int size = dPrev.size();
    std::sort(dPrev.begin(), dPrev.end());
    double dt0 = 0.0;
    if (size % 2 == 0) 
    {
        dt0 = (dPrev[size / 2] + dPrev[size/2 - 1]) / 2;
    } 
    else 
    {
        dt0 = dPrev[size / 2];
    }

    double dt1 = 0.0;
    int size1 = dCurr.size();
    std::sort(dCurr.begin(), dCurr.end());
    if (size1 % 2 == 0) 
    {
        dt1 = (dCurr[size1 / 2] + dCurr[size1/2 - 1]) / 2;
    } 
    else 
    {
        dt1 = dCurr[size1 / 2];
    }

    double dt = 1 / frameRate;
    TTC = dt1 * (dt / (dt0 - dt1));
}


void matchBoundingBoxes(std::vector<cv::DMatch> &matches, std::map<int, int> &bbBestMatches, DataFrame &prevFrame, DataFrame &currFrame)
{
    /*
        In this task, please implement the method "matchBoundingBoxes", which takes as input both the previous and the current data frames 
        and provides as output the ids of the matched regions of interest (i.e. the boxID property)“. Matches must be the ones with the 
        highest number of keypoint correspondences.

        The task is complete once the code is functional and returns the specified output, where each bounding box is assigned the match 
        candidate with the highest number of occurrences.
    */

    int queryFrameIdx, trainFrameIdx;
    cv::KeyPoint queryFrame, trainFrame;
    
    int prevSize = prevFrame.boundingBoxes.size();
    int currentSize = currFrame.boundingBoxes.size();
    int counts[prevSize][currentSize] = { };
    vector<int> prevBoxIds, currBoxIds;
    // matched keypoints 
    // The idea is to safe all the keypoints in a vector that are contained in the bounding box
    for(auto it1 = matches.begin(); it1 != matches.end(); ++it1 )
    {
        // save the id of every frame
        queryFrameIdx = (*it1).queryIdx;
        trainFrameIdx = (*it1).trainIdx;
        // we look into an specific frame
        queryFrame = prevFrame.keypoints[queryFrameIdx];
        trainFrame = currFrame.keypoints[trainFrameIdx];
        prevBoxIds.clear();
        currBoxIds.clear();
        // previous frame 
        for(auto it2 = prevFrame.boundingBoxes.begin(); it2!= prevFrame.boundingBoxes.end(); ++it2)
        {
            if((*it2).roi.contains(queryFrame.pt))
                prevBoxIds.push_back((*it2).boxID);
        }
        // current frame 
        for(auto it2 = currFrame.boundingBoxes.begin(); it2!= currFrame.boundingBoxes.end(); ++it2)
        {
            if((*it2).roi.contains(queryFrame.pt))
                currBoxIds.push_back((*it2).boxID);
        }
        // update the counter
        for(auto prevId:prevBoxIds)
        {
            for(auto currId:currBoxIds)
                counts[prevId][currId]++;
        }
    }
    // Best matches boxes
    for (int i = 0; i < prevSize; i++)
    {  
        int max_count = 0;
        int id_max = 0;
        for (int j = 0; j < currentSize; j++)
        {
            if (counts[i][j] > max_count)
            {  
                max_count = counts[i][j];
                id_max = j;
            }
        }
        bbBestMatches[i] = id_max;
    } 
}
