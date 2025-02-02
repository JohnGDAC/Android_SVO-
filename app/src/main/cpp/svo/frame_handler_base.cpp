// This file is part of SVO - Semi-direct Visual Odometry.
//
// Copyright (C) 2014 Christian Forster <forster at ifi dot uzh dot ch>
// (Robotics and Perception Group, University of Zurich, Switzerland).
//
// SVO is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or any later version.
//
// SVO is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <svo/abstract_camera.h>
#include <stdlib.h>
#include <Eigen/StdVector>
#include <fstream>
#include <svo/frame_handler_base.h>
#include <svo/config.h>
#include <svo/feature.h>
#include <svo/matcher.h>
#include <svo/map.h>
#include <svo/point.h>

namespace svo
{

// definition of global and static variables which were declared in the header
#ifdef SVO_TRACE
vk::PerformanceMonitor* g_permon = NULL;
#endif

FrameHandlerBase::FrameHandlerBase() :
  stage_(STAGE_PAUSED),
  set_reset_(false),
  set_start_(false),
  num_obs_last_(0),
  tracking_quality_(TRACKING_INSUFFICIENT)
{
#ifdef SVO_TRACE
  // Initialize Performance Monitor
  g_permon = new vk::PerformanceMonitor();
  g_permon->addTimer("pyramid_creation");
  g_permon->addTimer("sparse_img_align");
  g_permon->addTimer("reproject");
  g_permon->addTimer("reproject_kfs");
  g_permon->addTimer("reproject_candidates");
  g_permon->addTimer("feature_align");
  g_permon->addTimer("pose_optimizer");
  g_permon->addTimer("point_optimizer");
  g_permon->addTimer("local_ba");
  g_permon->addTimer("tot_time");
  g_permon->addLog("timestamp");
  g_permon->addLog("img_align_n_tracked");
  g_permon->addLog("repr_n_mps");
  g_permon->addLog("repr_n_new_references");
  g_permon->addLog("sfba_thresh");
  g_permon->addLog("sfba_error_init");
  g_permon->addLog("sfba_error_final");
  g_permon->addLog("sfba_n_edges_final");
  g_permon->addLog("loba_n_erredges_init");
  g_permon->addLog("loba_n_erredges_fin");
  g_permon->addLog("loba_err_init");
  g_permon->addLog("loba_err_fin");
  g_permon->addLog("n_candidates");
  g_permon->addLog("dropout");
  g_permon->init(Config::traceName(), Config::traceDir());
#endif

  SVO_INFO_STREAM("SVO initialized");
}

FrameHandlerBase::~FrameHandlerBase()
{
  SVO_INFO_STREAM("SVO destructor invoked");
#ifdef SVO_TRACE
  delete g_permon;
#endif
}

/**
 * 每一帧开始前的检查与上一帧内存清理
*/
bool FrameHandlerBase::startFrameProcessingCommon(const double timestamp)
{
  // start()置set_start_为true
  if(set_start_)
  {
    // 这里会置set_start_为false
    resetAll();
    stage_ = STAGE_FIRST_FRAME;
  }

  if(stage_ == STAGE_PAUSED)
    return false;

  SVO_LOG(timestamp);
  SVO_DEBUG_STREAM("New Frame");
  SVO_START_TIMER("tot_time");
  // timer_.start();

  // some cleanup from last iteration, can't do before because of visualization
  // 清理上一帧的内存
  map_.emptyTrash();
  return true;
}

/**
 * 每一帧结束处理
*/
int FrameHandlerBase::finishFrameProcessingCommon(
    const size_t update_id,
    const UpdateResult dropout,
    const size_t num_observations)
{
  SVO_LOG(dropout);

  // 上一帧特征点数量
  num_obs_last_ = num_observations;
  SVO_STOP_TIMER("tot_time");

#ifdef SVO_TRACE
  g_permon->writeToFile();
  {
    boost::unique_lock<boost::mutex> lock(map_.point_candidates_.mut_);
    size_t n_candidates = map_.point_candidates_.candidates_.size();
    SVO_LOG(n_candidates);
  }
#endif

  if(dropout == RESULT_FAILURE &&
      (stage_ == STAGE_DEFAULT_FRAME || stage_ == STAGE_RELOCALIZING ))
  {
    stage_ = STAGE_RELOCALIZING;
    tracking_quality_ = TRACKING_INSUFFICIENT;
  }
  else if (dropout == RESULT_FAILURE) {
      resetAll();
      set_start_ = true;
  }

  if(set_reset_)
    resetAll();

  return 0;
}

void FrameHandlerBase::resetCommon()
{
  map_.reset();
  stage_ = STAGE_PAUSED;
  set_reset_ = false;
  set_start_ = false;
  tracking_quality_ = TRACKING_INSUFFICIENT;
  num_obs_last_ = 0;
  SVO_INFO_STREAM("RESET");
}

/**
 * 优化后跟踪点个数，判断跟踪情况
*/
void FrameHandlerBase::setTrackingQuality(const size_t num_observations)
{
  tracking_quality_ = TRACKING_GOOD;
  if(num_observations < Config::qualityMinFts())
  {
    SVO_WARN_STREAM_THROTTLE(0.5, "Tracking less than %zu features!", Config::qualityMinFts());
    tracking_quality_ = TRACKING_INSUFFICIENT;
  }
  const int feature_drop = static_cast<int>(std::min(num_obs_last_, Config::maxFts())) - num_observations;
  if(feature_drop > Config::qualityMaxFtsDrop())
  {
    SVO_WARN_STREAM("Lost %d features!", feature_drop);
    tracking_quality_ = TRACKING_INSUFFICIENT;
  }
}

bool ptLastOptimComparator(Point* lhs, Point* rhs)
{
  return (lhs->last_structure_optim_ < rhs->last_structure_optim_);
}

/**
 * 优化当前帧的特征点对应的世界坐标点
 * 世界点，与观测帧集合建立重投影误差，优化
*/
void FrameHandlerBase::optimizeStructure(
    FramePtr frame,
    size_t max_n_pts,
    int max_iter)
{
  // 当前帧特征点对应3d点
  std::deque<Point*> pts;
  for(Features::iterator it=frame->fts_.begin(); it!=frame->fts_.end(); ++it)
  {
    if((*it)->point != NULL)
      pts.push_back((*it)->point);
  }
  max_n_pts = std::min(max_n_pts, pts.size());
  nth_element(pts.begin(), pts.begin() + max_n_pts, pts.end(), ptLastOptimComparator);
  for(std::deque<Point*>::iterator it=pts.begin(); it!=pts.begin()+max_n_pts; ++it)
  {
    // 对当前世界点的观测帧，优化相机平面重投影误差，更新当前世界点坐标
    (*it)->optimize(max_iter);
    (*it)->last_structure_optim_ = frame->id_;
  }
}


} // namespace svo
