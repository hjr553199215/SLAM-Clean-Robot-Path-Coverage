#include "CleaningPathPlanner.h"
#include <costmap_2d/cost_values.h>

CleaningPathPlanning::CleaningPathPlanning(costmap_2d::Costmap2DROS *costmap2d_ros)
{
    //temp solution.
    //costmap_2D提供的ROS化功能接口主要就是 costmap_2d::Costmap2DROS，它使用 costmap_2d::LayeredCostmap 来跟踪每一层。
    //每一层在 Costmap2DROS中以 插件方式被实例化，并被添加到 LayeredCostmap。 
    //每一层可以独立编译，且可使用C++接口实现对代价地图的随意修改，即LayerdCostmap为Costmap2DROS（用户接口）提供了加载地图层的插件机制，每个插件（即地图层）都是Layer类型的。 
    //costmap_2d::Costmap2D 类中实现了用来存储和访问2D代价地图的的基本数据结构。
    costmap2d_ros_ = costmap2d_ros;
    //costmap2d_ros_->updateMap();
    costmap2d_ = costmap2d_ros->getCostmap();
    //costmap2d_->saveMap("/home/wz/temp.pgm");


    ros::NodeHandle private_nh("~/cleaning_plan_nodehandle");
    plan_pub_ = private_nh.advertise<nav_msgs::Path>("cleaning_path", 1);//把路径公开
    grid_pub_ = private_nh.advertise<nav_msgs::OccupancyGrid>("covered_grid",1);//占用栅格地图打开


    string sizeOfCellString,coveredValueStr;

    SIZE_OF_CELL = 3;
    if(private_nh.searchParam("size_of_cell",sizeOfCellString))//搜索参数,根据名称"size of cell"搜索参数，将对应名称下的参数值赋给sizeOfCellString.
        private_nh.param("size_of_cell",SIZE_OF_CELL,3);//根据wiki的解释，这里在进行param操作前对SIZE_OF_CELL的赋值工作似乎不是很必要。

    GRID_COVERED_VALUE = 0;
    if(private_nh.searchParam("grid_covered_value",coveredValueStr))
        private_nh.param("grid_covered_value",GRID_COVERED_VALUE,0);


    int sizex = costmap2d_->getSizeInCellsX();//获取地图尺寸
    int sizey = costmap2d_->getSizeInCellsY();
    cout<<"The size of map is "<<sizex<<"  "<<sizey<<endl;
    resolution_ = costmap2d_->getResolution();//分辨率

    srcMap_=Mat(sizey,sizex,CV_8U);//映射成图片
    for(int r = 0; r < sizey; r++){
      for(int c = 0; c < sizex; c++ ){
          srcMap_.at<uchar>(r,c) = costmap2d_->getCost(c,sizey-r-1);//??sizey-r-1 caution: costmap's origin is at left bottom ,while opencv's pic's origin is at left-top.
          //getCost（）:获取代价值         
      }
    }

       initializeMats(); 
       initializeCoveredGrid();//赋值到栅格地图中

    //imshow("debugMapImage",srcMap_);
    //imshow("debugCellMatImage",cellMat_);
    //waitKey(0);
    //imwrite("debug_srcmap.jpg",srcMap_);

    if(!srcMap_.empty())initialized_ = true;//这句话説明srcMap_里面得有东西才能说明初始化成功。
    else initialized_ = false;
}

vector<geometry_msgs::PoseStamped> CleaningPathPlanning::GetPathInROS()
{
//    vector<geometry_msgs::PoseStamped> resultVec;
    if(!pathVecInROS_.empty()) pathVecInROS_.clear();//清空操作,用于存放机器人期望运动轨迹
    geometry_msgs::PoseStamped posestamped;
    geometry_msgs::Pose pose;
    vector<cellIndex> cellvec;
    cellvec = GetPathInCV();//获取图像中需要的路径
    /*trasnsform*/
    vector<cellIndex>::iterator iter;//cellIndex里面存放的是行，列以及角度信息。
    int sizey = cellMat_.rows;

    for(iter=cellvec.begin(); iter!=cellvec.end();iter++)
    {
         costmap2d_->mapToWorld((*iter).col * SIZE_OF_CELL + SIZE_OF_CELL/2 , (sizey-(*iter).row-1)*SIZE_OF_CELL + SIZE_OF_CELL/2, pose.position.x, pose.position.y);//worldToMap将world坐标点映射到map
         pose.orientation.w = cos((*iter).theta * PI / 180 / 2); //(sizey-(*iter).row-1)
         pose.orientation.x = 0;
         pose.orientation.y = 0;
         pose.orientation.z = sin((*iter).theta * PI / 180 / 2);
         posestamped.header.stamp= ros::Time::now();
         posestamped.header.frame_id = "map";
         posestamped.pose = pose;

         pathVecInROS_.push_back(posestamped);
    }
    publishPlan(pathVecInROS_);//发布路径信息
    cout<<"The path size is "<<pathVecInROS_.size()<<endl;
    return pathVecInROS_;
}

vector<geometry_msgs::PoseStamped> CleaningPathPlanning::GetBorderTrackingPathInROS()
{
     //vector<geometry_msgs::PoseStamped> resultPathInROS;
     if(!pathVecInROS_.empty())pathVecInROS_.clear();
     geometry_msgs::PoseStamped posestamped;
     geometry_msgs::Pose pose;
     vector<cv::Point2i> resultCV;
     GetBorderTrackingPathInCV(resultCV);
     vector<bool> visitedVec;

     cv::Point2i lastPoint = resultCV[0], curPoint;
     double theta = 0.0;
     visitedVec.assign(resultCV.size(),false);
     int i,j,k,index;
     for(i = 0; i < resultCV.size(); i++)
     {
         if(!visitedVec[i])
         {
             for(k=-SIZE_OF_CELL/2;k<SIZE_OF_CELL/2;k++)
             {
                 for(j=-SIZE_OF_CELL/2;j<SIZE_OF_CELL/2;j++)
                 {
                     if(findElement(resultCV,Point2i(resultCV[i].x+j,resultCV[i].y+k),index))
                     {
                         visitedVec[index] = true;

                         curPoint = resultCV[index];
                         if(curPoint.x==lastPoint.x)
                         {
                             if(curPoint.y>lastPoint.y)theta=PI/2;
                             else theta = -PI/2;
                         }
                         else theta = atan((curPoint.y-lastPoint.x)/(curPoint.x-lastPoint.x));

                         //srcMap_'s shape is same with costmap2d_,ie.. they have same resolution and size.
                         costmap2d_->mapToWorld(resultCV[index].x,srcMap_.rows-resultCV[index].y-1,pose.position.x, pose.position.y);
                         pose.orientation.w = cos(theta * PI / 180 / 2);
                         pose.orientation.x = 0;
                         pose.orientation.y = 0;
                         pose.orientation.z = sin(theta * PI / 180 / 2);
                         posestamped.header.stamp= ros::Time::now();
                         posestamped.header.frame_id = "map";
                         posestamped.pose = pose;

                         pathVecInROS_.push_back(posestamped);
                     }
                 }
             }
         }
     }
     publishPlan(pathVecInROS_);
     return pathVecInROS_;
}


//首先使用opencv工具在原分辨率下进行边界路径规划。然后转换结果
//采用ROS格式，并考虑机器人的形状。
void CleaningPathPlanning::GetBorderTrackingPathInCV(vector<cv::Point2i> &resultVec)
{
    std::vector<cv::Point2i> borderPointsIndexVec;//todo:make point2i corrresponding to cellindex.
    resultVec.clear();
    if(srcMap_.empty())return;//srcMap 是个啥？

    cv::Point2i temppoint;
    int r,c,i,j;
    for( r=0; r<srcMap_.rows; r++)
    {
        for( c=0;c<srcMap_.cols;c++)
        {
            //ROS_INFO("The value at row %d, col %d is %d",r,c,srcMap_.at<uchar>(r,c));
            if(srcMap_.at<uchar>(r,c) == costmap_2d::INSCRIBED_INFLATED_OBSTACLE)
            {
                ROS_INFO("entered once!");
                if(srcMap_.at<uchar>(r,c-1)!=srcMap_.at<uchar>(r,c+1)
                        && (srcMap_.at<uchar>(r,c-1) == costmap_2d::FREE_SPACE
                           || srcMap_.at<uchar>(r,c+1)== costmap_2d::FREE_SPACE))
                {
                    temppoint.x = c;
                    temppoint.y = r;
                    borderPointsIndexVec.push_back(temppoint);
                }
                else if(srcMap_.at<uchar>(r-1,c)!=srcMap_.at<uchar>(r+1,c)
                        &&(srcMap_.at<uchar>(r-1,c) == costmap_2d::FREE_SPACE
                           || srcMap_.at<uchar>(r+1,c)== costmap_2d::FREE_SPACE) )
                {
                    temppoint.x = c;
                    temppoint.y = r;
                    borderPointsIndexVec.push_back(temppoint);
                }
            }
        }
    }

    std::cout<<"The border path length is:"<<borderPointsIndexVec.size()<<std::endl;
    std::vector<bool> visitedVec;
    if(borderPointsIndexVec.empty())return;

    int minDistIndex=-1,loopCounter=0;
    double dist,minDist;
    visitedVec.assign(borderPointsIndexVec.size(),false);//vector assign是vector容器下的赋值函数。

    Point2i curPoint = borderPointsIndexVec[0];
    visitedVec[0]=true;

    while(loopCounter < borderPointsIndexVec.size())
    {
        minDist = 1e6;
        resultVec.push_back(curPoint);
        for(j=1;j<borderPointsIndexVec.size();j++)
        {
            if(!visitedVec[j])
            {
                dist = distance(curPoint,borderPointsIndexVec[j]);
                if(dist < minDist)
                {
                    minDist=dist;
                    minDistIndex=j;
                }
            }
        }
        curPoint = borderPointsIndexVec[minDistIndex];
        visitedVec[minDistIndex] = true;
        loopCounter++;
    }

    //Mat resultmat=Mat(srcMap_.rows,srcMap_.cols, CV_8UC3);;
    //writeResult(resultmat,resultVec);
}

void CleaningPathPlanning::SetCoveredGrid(double wx, double wy)
{
    unsigned int mx,my,index;
    bool isok = costmap2d_->worldToMap(wx,wy,mx,my);
    if(!isok)
    {
        return;
    }

    for(int dx = -SIZE_OF_CELL/2; dx < SIZE_OF_CELL/2+1; dx++)
     {
        for(int dy =-SIZE_OF_CELL/2; dy<SIZE_OF_CELL/2+1; dy++)
        {
            index = costmap2d_->getIndex(mx+dx,my+dy);
            covered_path_grid_.data[index] = GRID_COVERED_VALUE;
        }
    }
}

void CleaningPathPlanning::PublishGrid()
{
    if(!initialized_)initializeCoveredGrid();
    grid_pub_.publish(covered_path_grid_);
}

vector<cellIndex> CleaningPathPlanning::GetPathInCV()//在地图中获取路径
{
    mainPlanningLoop();
    return this->pathVec_;
}

void CleaningPathPlanning::PublishCoveragePath(){
  publishPlan(this->pathVecInROS_);
}

void CleaningPathPlanning::publishPlan(const std::vector<geometry_msgs::PoseStamped> &path)
{
    if (!initialized_) {
        ROS_ERROR(
           "This planner has not been initialized yet, but it is being used, please call initialize() before use");
        return;
    }

    //create a message for the plan
    nav_msgs::Path gui_path;
    gui_path.poses.resize(path.size());

    gui_path.header.frame_id = "map";
    gui_path.header.stamp = ros::Time::now();

    // Extract the plan in world co-ordinates, we assume the path is all in the same frame
    for (unsigned int i = 0; i < path.size(); i++) {
        gui_path.poses[i] = path[i];
    }

    plan_pub_.publish(gui_path);
}

bool CleaningPathPlanning::cellContainsPoint(Point2i pt, cellIndex cell)
{
    return pt.y>cell.row*SIZE_OF_CELL && pt.y<(cell.row+1)*SIZE_OF_CELL
            && pt.x>cell.col*SIZE_OF_CELL && pt.x<(cell.col+1)*SIZE_OF_CELL;
}

bool CleaningPathPlanning::initializeMats()
{
    //初始化成员变量。
    if(srcMap_.empty())return false;
    getCellMatAndFreeSpace(srcMap_,cellMat_,freeSpaceVec_);//创建栅格地图，按照栅格大小来

    neuralizedMat_ = Mat(cellMat_.rows,cellMat_.cols,CV_32F);//含有地图障碍物信息的costmap
    Astarmap = Mat(cellMat_.rows,cellMat_.cols,CV_32F);//a*矩阵
    initializeNeuralMat(cellMat_,neuralizedMat_);
    Astarmap = neuralizedMat_;
    return true;
}

void CleaningPathPlanning::getCellMatAndFreeSpace(Mat srcImg, Mat &cellMat,vector<cellIndex> &freeSpaceVec)
{
    cellMat = Mat(srcImg.rows / SIZE_OF_CELL,srcImg.cols / SIZE_OF_CELL, srcImg.type());//cellMat是以之前规定的cell为最小单位重新划分的矩阵

    freeSpaceVec.clear();
    bool isFree = true;
    int r = 0,c = 0, i = 0,j = 0;
    for (r = 0;r < cellMat.rows; r++ )
    {
        for(c = 0 ; c<cellMat.cols; c++)
        {
            isFree = true;//是否存在障碍物
            for(i = 0; i<SIZE_OF_CELL; i++)
            {
                for(j = 0; j < SIZE_OF_CELL; j++)
                {
                    if(srcImg.at<uchar>(r*SIZE_OF_CELL+i,c*SIZE_OF_CELL+j) != costmap_2d::FREE_SPACE)
                    {
                        isFree=false;
                        i = SIZE_OF_CELL;
                        break;
                    }
                }
            }
            if(isFree)
            {
                cellIndex ci;
                ci.row = r;
                ci.col = c;
                ci.theta = 0;
                freeSpaceVec.push_back(ci);//存放所有空白的向量
                cellMat.at<uchar>(r,c) = costmap_2d::FREE_SPACE;//0
            }
            else{cellMat.at<uchar>(r,c) = costmap_2d::LETHAL_OBSTACLE;}//254
        }
    }
    cout<<"freespace size:"<< freeSpaceVec.size()<<endl;
    //imwrite("cellMat.jpg",cellMat);
    return;
}

void CleaningPathPlanning::initializeNeuralMat(Mat cellMat, Mat neuralizedMat)
{
    int i = 0,j = 0;
    for(i = 0; i < neuralizedMat.rows; i++)
    {
        for(j = 0;j< neuralizedMat.cols; j++ )
        {
            if(cellMat.at<uchar>(i,j) == costmap_2d::LETHAL_OBSTACLE) neuralizedMat.at<float>(i,j) = -100000.0;//hjr注
            else neuralizedMat.at<float>(i,j) = 50.0 /j;//这里的1.0/j的用意是什么？这里有没有考虑到j=0时刻的问题呢？   hjr注
        }
    }
    return;
}


void CleaningPathPlanning::writeResult(Mat resultmat,vector<cellIndex> pathVec)
{
    int i = 0,j = 0;
    Point initpoint = Point(pathVec[0].col*SIZE_OF_CELL+SIZE_OF_CELL/2,pathVec[0].row*SIZE_OF_CELL+SIZE_OF_CELL/2);//这里是取中心位置的意思吗？
    for(i = 1; i<pathVec.size();i++)
    {
        Point cupoint = Point(pathVec[i].col*SIZE_OF_CELL+SIZE_OF_CELL/2,pathVec[i].row*SIZE_OF_CELL+SIZE_OF_CELL/2);
        if(sqrt((initpoint.x-cupoint.x)*(initpoint.x-cupoint.x)+(initpoint.y-cupoint.y)*(initpoint.y-cupoint.y))>2)
        {
            line(resultmat,initpoint,cupoint,Scalar(0,255,0),0.3,8);//line就是划线函数
        }
        else line(resultmat,initpoint,cupoint,Scalar(0,0,255),0.5);
        initpoint = cupoint;
        cout << "The point of step "<<i<<" is: "<<pathVec[i].row<<" "<<pathVec[i].col<<endl;
        /*resultMat.at<Vec3b>(pathVec[i].row*SIZE_OF_CELL,pathVec[i].col*SIZE_OF_CELL)[0] = 0;
        resultMat.at<Vec3b>(pathVec[i].row*SIZE_OF_CELL,pathVec[i].col*SIZE_OF_CELL)[1] = 0;
        resultMat.at<Vec3b>(pathVec[i].row*SIZE_OF_CELL,pathVec[i].col*SIZE_OF_CELL)[2] = 255;*/
    }
  //    imshow("resultMat",resultmat);
   //    waitKey(0);
    imwrite("reaultMat.jpg",resultmat);
}

void CleaningPathPlanning::writeResult(Mat resultmat,vector<cv::Point2i> pathVec)
{
    int i = 0,j = 0;
    Point initpoint = pathVec[0];
    for(i = 1; i<pathVec.size();i++)
    {
        Point cupoint = pathVec[i];
        line(resultmat,initpoint,cupoint,Scalar(0,0,255),0.5);
        initpoint = cupoint;
        //std::cout<<"X: "<<cupoint.x<<","<<"Y:"<<cupoint<<std::endl;

    }
  //    imshow("resultMat",resultmat);
  //    waitKey(0);
}

void CleaningPathPlanning::mainPlanningLoop()
{
    cellIndex initPoint,nextPoint, currentPoint;////cellIndex里面存放的是行，列以及角度信息。
    //    initPoint.row = cellMat_.rows/2; //initPoint to be made interface.
    //    initPoint.col = cellMat_.cols/2;

    initPoint.theta = 90;
    bool isok = costmap2d_ros_->getRobotPose(initPose_);
    if(!isok)
    {
        ROS_INFO("Failed to get robot location! Please check where goes wrong!");
        return;
    }
    //initPoint.row = initPose_.getOrigin().y()
    unsigned int mx,my;
    double wx = initPose_.getOrigin().x();//获取原点的x坐标？这里还需要再搞清楚
    double wy = initPose_.getOrigin().y();
    //geometry_msgs::PoseStamped current_position;
    //tf::poseStampedTFToMsg(global_pose, current_position);

    bool getmapcoor = costmap2d_->worldToMap(wx,wy,mx,my);//全局地图映射到局部中
    if(!getmapcoor)
    {
        ROS_INFO("Failed to get robot location in map! Please check where goes wrong!");
        return;
    }
    initPoint.row = cellMat_.rows - my/SIZE_OF_CELL - 1;//再研究一下这个行列之间的转换问题。
    initPoint.col = mx/SIZE_OF_CELL;//这里貌似不光有行列转换


    currentPoint = initPoint;//将初始化的点赋予给当前点。
    pathVec_.clear();
    pathVec_.push_back(initPoint);

    float initTheta = initPoint.theta; //initial orientation
    const float c_0 = 50;//这貌似是后面算法中要用到的参数。0.001
    float e = 0.0, v = 0.0, v_1=0.0, deltaTheta = 0.0, lasttheta = initTheta, PI = 3.14159;
    int fx;
    vector<float> thetaVec = {0, 45,90,135,180,225,270,315};
    //vector<float> thetaVec = {0,315,270,225,180,135,90,45};
    //the main planning loop
    //while(freeSpaceVec_.size()>0)
    
    
    
    
    for(int loop=0;loop<9000;loop++)
    {
        //erase current point from free space first.
        vector<cellIndex>::iterator it;
        
	
	
	/*
	for(it=freeSpaceVec_.begin();it!=freeSpaceVec_.end();)
        {
            if((*it).row==nextPoint.row && (*it).col==nextPoint.col)
            {it = freeSpaceVec_.erase(it);continue;}
            it++;
        }

        */
        
        
        
        //compute neiborhood's activities
        //hjr注：目前我认为这里进行的是有关方向上的抉择。
        int maxIndex = 0;//参数索引，来获取当前最优方向
        float max_v = -300;//用来判断是否路径可行
        neuralizedMat_.at<float>(currentPoint.row ,currentPoint.col) = -250.0;//将mat中对应元素赋值，neuralizedMat_是使用initializeNeuralMat发布的地图，目前-250为走过的路径
	    lasttheta = currentPoint.theta;
        for(int id = 0; id < 8; id++)
        {
            deltaTheta = max(thetaVec[id],lasttheta)-min(thetaVec[id],lasttheta);//8个方向判断，保证为正数
            if(deltaTheta>180) deltaTheta=360-deltaTheta;
            e = 1 - abs(deltaTheta) / 180;//计算参数，反向则速度设为0
            switch (id)
            {
                case 0:
                    if(currentPoint.col==neuralizedMat_.cols-1){v=-100000;break;}//处于边界，及时停止向0度方向运动
                    v = neuralizedMat_.at<float>(currentPoint.row ,currentPoint.col+1) + c_0 * e;
		    
                    break;
                case 1:
                if(currentPoint.col==neuralizedMat_.cols-1 || currentPoint.row == 0){v=-100000;break;}//处于边界，仍然往右上运动
                    v = neuralizedMat_.at<float>(currentPoint.row-1 ,currentPoint.col+1) + c_0 * e-200;

                    break;
                case 2:
                if(currentPoint.row == 0){v=-100000;break;}//处于边界，仍然往上运动
                    v = neuralizedMat_.at<float>(currentPoint.row-1 ,currentPoint.col) + c_0 * e;

                    break;
                case 3:
                if(currentPoint.col== 0 || currentPoint.row == 0){v=-100000;break;}
                    v = neuralizedMat_.at<float>(currentPoint.row-1 ,currentPoint.col-1) + c_0 * e-200;

                    break;
                case 4:
                if(currentPoint.col== 0){v=-100000;break;}
                    v = neuralizedMat_.at<float>(currentPoint.row ,currentPoint.col-1) + c_0 * e;

                    break;
                case 5:
                if(currentPoint.col== 0 || currentPoint.row == neuralizedMat_.rows-1){v=-100000;break;}
                    v = neuralizedMat_.at<float>(currentPoint.row+1 ,currentPoint.col-1) + c_0 * e-200;

                    break;
                case 6:
                if(currentPoint.row == neuralizedMat_.rows-1){v=-100000;break;}
                    v = neuralizedMat_.at<float>(currentPoint.row+1 ,currentPoint.col) + c_0 * e;

                    break;
                case 7:
                if(currentPoint.col==neuralizedMat_.cols-1 || currentPoint.row == neuralizedMat_.rows-1){v=-100000;break;}
                    v = neuralizedMat_.at<float>(currentPoint.row+1 ,currentPoint.col+1) + c_0 * e-200;

                    break;
                default:
                    break;
            }
            if(v > max_v)//判断是否可走，如果该位置方向已经走过则不会触发
            {
                max_v = v;
                maxIndex=id;
            }
            
           
            
            if(v== max_v&&id>maxIndex)//更偏向id更大
	    {
	     max_v = v;
	     maxIndex = id;
	    }

        }

         
        if(max_v <= 0)//接下来应该是在处理距离上的关系。
        {
            float dist = 0.0, min_dist = 100000000;
            //vector<cellIndex>::iterator min_iter;
            int ii=0, min_index=-1;
            for(it=freeSpaceVec_.begin();it!=freeSpaceVec_.end();it++)//获取所有空白向量
            {
                if(neuralizedMat_.at<float>((*it).row,(*it).col) > 0)
                { 
		  if(Boundingjudge((*it).row,(*it).col))//周围是否存在-250的点（八个方向上）
		  {
                    dist = sqrt((currentPoint.row-(*it).row)*(currentPoint.row-(*it).row)+(currentPoint.col-(*it).col)*(currentPoint.col-(*it).col));
		    if(dist < min_dist)
                    {
                        min_dist = dist;//选取最小的路径点
                        min_index = ii;//对应的索引信息
                    }
		  }
                }
                ii++;
            }
            //if(min_dist==0 || min_index == -1)
            //{break;}
            //else
            if(min_index!=-1&&min_dist!=100000000)
             {
                cout << "next point index: "<<min_index<< endl;
                cout << "distance: "<<min_dist << endl;
                nextPoint = freeSpaceVec_[min_index];//拿出下一个点的位置
                currentPoint = nextPoint;
                pathVec_.push_back(nextPoint);//保存路径

                continue; 
             }
             else //产生了自锁现象
	     {
	        ROS_INFO("The program has been dead because of the self-locking");
		ROS_ERROR("The program has gone through %d steps", pathVec_.size());
	       break;
	     }
        }
        
        
        //hjr自己编写部分------------------------------------------------------------------------------


        //next point.
        switch (maxIndex)
        {
        case 0:
            nextPoint.row = currentPoint.row;
            nextPoint.col = currentPoint.col+1;
            break;
        case 1:
            nextPoint.row = currentPoint.row-1;
            nextPoint.col = currentPoint.col+1;
            break;
        case 2:
            nextPoint.row = currentPoint.row-1;
            nextPoint.col = currentPoint.col;
            break;
        case 3:
            nextPoint.row = currentPoint.row-1;
            nextPoint.col = currentPoint.col-1;
            break;
        case 4:
            nextPoint.row = currentPoint.row;
            nextPoint.col = currentPoint.col-1;
            break;
        case 5:
            nextPoint.row = currentPoint.row+1;
            nextPoint.col = currentPoint.col-1;
            break;
        case 6:
            nextPoint.row = currentPoint.row+1;
            nextPoint.col = currentPoint.col;
            break;
        case 7:
            nextPoint.row = currentPoint.row+1;
            nextPoint.col = currentPoint.col+1;
            break;
        default:
            break;
        }
        nextPoint.theta = thetaVec[maxIndex];
        currentPoint = nextPoint;
        pathVec_.push_back(nextPoint);
	
	
	 //for(it=freeSpaceVec_.begin();it!=freeSpaceVec_.end();)
       // {
       //     if((*it).row==nextPoint.row && (*it).col==nextPoint.col)
       //     {it = freeSpaceVec_.erase(it);continue;}
      //      it++;
      //  }
    }
    
    

    Mat resultMat = Mat(srcMap_.rows,srcMap_.cols, CV_8UC3);
    writeResult(resultMat,pathVec_);
}

double CleaningPathPlanning::distance(Point2i pta, Point2i ptb)
{
    return sqrt((pta.x-ptb.x)*(pta.x-ptb.x)+(pta.y-ptb.y)*(pta.y-ptb.y));
}

bool CleaningPathPlanning::findElement(vector<Point2i> pointsVec, Point2i pt, int &index)
{
    for(int i = 0;i<pointsVec.size();i++)
    {
        if(pointsVec[i].x == pt.x && pointsVec[i].y == pt.y)
           { index = i;
        return true;}
    }
    index = -1;
    return false;
}



bool CleaningPathPlanning::initializeCoveredGrid()//在这里我对CoverGrid的理解为覆盖栅格。
{
    boost::unique_lock<costmap_2d::Costmap2D::mutex_t> lock(*(costmap2d_->getMutex()));//costmap互斥锁
    double resolution = costmap2d_->getResolution();//分辨率

    covered_path_grid_.header.frame_id = "map";//covered_path_grid_是costmap库中的占据栅格地图消息。
    covered_path_grid_.header.stamp = ros::Time::now();
    covered_path_grid_.info.resolution = resolution;

    covered_path_grid_.info.width = costmap2d_->getSizeInCellsX();
    covered_path_grid_.info.height = costmap2d_->getSizeInCellsY();

    double wx, wy;
    costmap2d_->mapToWorld(0, 0, wx, wy);//从地图坐标系转换至世界坐标系，start_x, start_y，wx, wy
    covered_path_grid_.info.origin.position.x = wx - resolution / 2;
    covered_path_grid_.info.origin.position.y = wy - resolution / 2;
    covered_path_grid_.info.origin.position.z = 0.0;
    covered_path_grid_.info.origin.orientation.w = 1.0;

    covered_path_grid_.data.resize(covered_path_grid_.info.width * covered_path_grid_.info.height);//这里可以理解为一共有多少个栅格，所以长×宽

    unsigned char* data = costmap2d_->getCharMap();//返回一个指针，指向一个底层无符号字符数组，数组中存储着代价值，这个数组貌似还可以用来作为代价地图。
    for (unsigned int i = 0; i < covered_path_grid_.data.size(); i++)
    {
        /*if(data[i]==costmap_2d::FREE_SPACE)
            covered_path_grid_.data[i] = costmap_2d::FREE_SPACE;
        else
            covered_path_grid_.data[i] = 0;*/
        covered_path_grid_.data[i] = data[i];//这里我理解为将代价值赋予到栅格地图的每个对应栅格当中去。
    }
    return true;
}
    
    
//hjr---------------------------------------------------------
bool CleaningPathPlanning::Boundingjudge(int a,int b)
{
  int num=0;
  for(int i=-1;i<=1;i++)
  {
    for(int m=-1;m<=1;m++)
    {
      if(i==0&&m==0) 
      {
	continue;
      }
      if(neuralizedMat_.at<float>((a+i),(b+m))==-250.0)
      num++;
    }
  }
  if(num!=0)
    return true;
  else
    return false;
}

bool CleaningPathPlanning::outcloselist(int a, int b)
{
vector<Astar>::iterator it;
 for(it=closelist.begin();it!=closelist.end();)
       {
            if((*it).row==a && (*it).col==b)
            {
	     return false;
	      break;
	    }
           it++;
       }
       return true;
}



bool CleaningPathPlanning::outopenlist(int a, int b)
{
    vector<Astar>::iterator it;
    for(it=openlist.begin();it!=openlist.end();)
       {
            if((*it).row==a && (*it).col==b)
            {
	      G_compare = (*it).G;
	     return false;
	      break;
	    }
           it++;
       }
       return true;
}


void CleaningPathPlanning::Astar_find_path(int a, int b, int c, int d)
{
   int F=0;
   int G=0;
   int H=0;
   
   int min_F =100000;
   bool SW = false;
  
   Astar initial;
   Astar temp;
   Astar current;
   Astar next;
   initial.row = a;
   initial.col  = b;
   initial.father_row=a;
   initial.father_col=b;
   initial.F = 0;
   initial.G = 0;
   initial.H = 0;
   //----------------------------------------------------------
   openlist.push_back(initial);//加入初始点
   //------------------------------------------------
   
   vector<Astar>::iterator ite;
 for(ite=openlist.begin();ite!=openlist.end();)
 {
   if((*ite).F<=min_F)
   {
     min_F = (*ite).F;
     current = (*ite);
   }
   ite++;
 }
 
 
 
 //--------------------------------------------------
vector<Astar>::iterator it;

 for(it=openlist.begin();it!=openlist.end();)
       {
            if((*it).row==current.row && (*it).col==current.col)
            {
	      closelist.push_back((*it));
	      openlist.erase(it);
	      break;
	    }
           it++;
       }

 //-------------------------------------------------------
   for(int i=-1;i<=1;i++)
  {
    for(int m=-1;m<=1;m++)
    {
      if(i==0&&m==0) 
      {
	continue;
      }
     if(Astarmap.at<float>((current.row+i),(current.col+m))!=-100000.0||outcloselist((current.row+i),(current.col+m)))
     {
       if(!outopenlist((current.row+i),(current.col+m)))
       {
	 if(i==0||m==0)
	 {
	   if((current.G+10)<G_compare)//G值更小，説明更好.
	   {
              temp.father_row = current.row;
              temp.father_col =current.col;
	      temp.G = current.G+10;
	   }
	 }
	 else
	 {
	     if((current.G+14)<G_compare)//G值更小，説明更好.
	   {
              temp.father_row = current.row;
              temp.father_col =current.col;
	      temp.G = current.G+14;
	   }
	 }
	 
       } 
       temp.row = current.row+i;
       temp.col =current.col+m;
       temp.father_row = current.row;
       temp.father_col =current.col;
       
       
       if(i==0||m==0)
       {
	 temp.G = 10+current.G;
       }
       else
       {
	 temp.G = 14+current.G;
       }
        
        temp.H = (abs(c-temp.row) + abs(d-temp.col))*10;
	temp.F = temp.G + temp.H;
       
       openlist.push_back(temp);
     }
    }
  }
  
 
//--------------------------------------------------
// vector<Astar>::iterator it;
// 
//  for(it=openlist.begin();it!=openlist.end();)
//        {
//             if((*it).row==current.row && (*it).col==current.col)
//             {
// 	      closelist.push_back((*it));
// 	      openlist.erase(it);
// 	      break;
// 	    }
//            it++;
//        }

 //------------------------------------------------------------找到需要的点了吗？
 for(it=openlist.begin();it!=openlist.end();)
       {
            if((*it).row==c && (*it).col==d)
            {
	      SW = false;
	      break;
	    }
           it++;
       }
 
}
