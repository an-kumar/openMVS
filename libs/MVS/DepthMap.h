/*
* DepthMap.h
*
* Copyright (c) 2014-2015 SEACAVE
*
* Author(s):
*
*      cDc <cdc.seacave@gmail.com>
*
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU Affero General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Affero General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*
* Additional Terms:
*
*      You are required to preserve legal notices and author attributions in
*      that material or in the Appropriate Legal Notices displayed by works
*      containing it.
*/

#ifndef _MVS_DEPTHMAP_H_
#define _MVS_DEPTHMAP_H_


// I N C L U D E S /////////////////////////////////////////////////

#include "Image.h"
#include "PointCloud.h"


// D E F I N E S ///////////////////////////////////////////////////

#define ComposeDepthFilePathBase(b, i, e) MAKE_PATH(String::FormatString((b + "%04u." e).c_str(), i))
#define ComposeDepthFilePath(i, e) MAKE_PATH(String::FormatString("depth%04u." e, i))


// S T R U C T S ///////////////////////////////////////////////////

namespace MVS {

DECOPT_SPACE(OPTDENSE)

namespace OPTDENSE {
enum DepthFlags {
	REMOVE_SPECKLES	= (1 << 0),
	FILL_GAPS		= (1 << 1),
	ADJUST_FILTER	= (1 << 2),
	OPTIMIZE		= (REMOVE_SPECKLES|FILL_GAPS)
};
extern unsigned nMinResolution;
extern unsigned nResolutionLevel;
extern unsigned nMinViews;
extern unsigned nMaxViews;
extern unsigned nMinViewsFilter;
extern unsigned nMinViewsFilterAdjust;
extern unsigned nMinViewsTrustPoint;
extern unsigned nNumViews;
extern bool bAddCorners;
extern float fViewMinScore;
extern float fViewMinScoreRatio;
extern float fMinAngle;
extern float fOptimAngle;
extern float fMaxAngle;
extern float fDescriptorMinMagnitudeThreshold;
extern float fDepthDiffThreshold;
extern float fPairwiseMul;
extern float fPointFilter;
extern float fOptimizerEps;
extern int nOptimizerMaxIters;
extern unsigned nSpeckleSize;
extern unsigned nIpolGapSize;
extern unsigned nOptimize;
extern unsigned nEstimateColors;
extern unsigned nEstimateNormals;
extern float fNCCThresholdKeep;
extern float fNCCThresholdRefine;
extern unsigned nEstimationIters;
extern unsigned nRandomIters;
extern unsigned nRandomMaxScale;
extern float fRandomDepthRatio;
extern float fRandomAngle1Range;
extern float fRandomAngle2Range;
extern float fRandomSmoothDepth;
extern float fRandomSmoothNormal;
extern float fRandomSmoothBonus;
} // namespace OPTDENSE
/*----------------------------------------------------------------*/


typedef float Depth;
typedef Point3f Normal;
typedef TImage<Depth> DepthMap;
typedef TImage<Normal> NormalMap;
typedef TImage<float> ConfidenceMap;
typedef SEACAVE::cList<Depth,Depth,0> DepthArr;
typedef SEACAVE::cList<DepthMap,const DepthMap&,2> DepthMapArr;
typedef SEACAVE::cList<NormalMap,const NormalMap&,2> NormalMapArr;
typedef SEACAVE::cList<ConfidenceMap,const ConfidenceMap&,2> ConfidenceMapArr;
/*----------------------------------------------------------------*/


struct DepthData {
	struct ViewData {
		float scale; // image scale relative to the reference image
		Camera camera; // camera matrix corresponding to this image
		Image32F image; // image float intensities
		Image* pImageData; // image data

		template <typename IMAGE>
		static bool ScaleImage(const IMAGE& image, IMAGE& imageScaled, float scale) {
			if (ABS(scale-1.f) < 0.15f)
				return false;
			cv::resize(image, imageScaled, cv::Size(), scale, scale, cv::INTER_LINEAR);
			return true;
		}
	};
	typedef SEACAVE::cList<ViewData,const ViewData&,2> ViewDataArr;

	ViewDataArr images; // array of images used to compute this depth-map (reference image is the first)
	ViewScoreArr neighbors; // array of all images seeing this depth-map (ordered by decreasing importance)
	IndexArr points; // indices of the sparse 3D points seen by the this image
	BitMatrix mask; // mark pixels to be ignored
	DepthMap depthMap; // depth-map
	NormalMap normalMap; // normal-map in camera space
	ConfidenceMap confMap; // confidence-map
	float dMin, dMax; // global depth range for this image
	unsigned references; // how many times this depth-map is referenced (on 0 can be safely unloaded)
	CriticalSection cs; // used to count references

	inline DepthData() : references(0) {}

	inline void ReleaseImages() {
		FOREACHPTR(ptrImage, images)
			ptrImage->image.release();
	}
	inline void Release() {
		depthMap.release();
		normalMap.release();
		confMap.release();
	}

	inline bool IsValid() const {
		return !images.IsEmpty();
	}
	inline bool IsEmpty() const {
		return depthMap.empty();
	}

	void GetNormal(const ImageRef& ir, Point3f& N, const TImage<Point3f>* pPointMap=NULL) const;
	void GetNormal(const Point2f& x, Point3f& N, const TImage<Point3f>* pPointMap=NULL) const;

	bool Save(const String& fileName) const;
	bool Load(const String& fileName);

	unsigned GetRef();
	unsigned IncRef(const String& fileName);
	unsigned DecRef();

	#ifdef _USE_BOOST
	// implement BOOST serialization
	template<class Archive>
	void serialize(Archive& ar, const unsigned int /*version*/) {
		ASSERT(IsValid());
		ar & depthMap;
		ar & normalMap;
		ar & confMap;
		ar & dMin;
		ar & dMax;
	}
	#endif
};
typedef SEACAVE::cList<DepthData,const DepthData&,1> DepthDataArr;
/*----------------------------------------------------------------*/


struct DepthEstimator {
	static const int TexelChannels = 1;
	static const int nSizeHalfWindow = 3;
	static const int nSizeWindow = nSizeHalfWindow*2+1;
	static const int nTexels = nSizeWindow*nSizeWindow*TexelChannels;

	enum ENDIRECTION {
		LT2RB = 0,
		RB2LT,
		DIRS
	};

	typedef TPoint2<uint16_t> MapRef;
	typedef CLISTDEF0(MapRef) MapRefArr;

	typedef Eigen::Matrix<float,nTexels,1> TexelVec;
	struct NeighborData {
		Depth depth;
		Normal normal;
		inline NeighborData() {}
		inline NeighborData(Depth d, const Normal& n) : depth(d), normal(n) {}
	};

	struct ViewData {
		const DepthData::ViewData& view;
		const Matrix3x3 Hl;   //
		const Vec3 Hm;	      // constants during per-pixel loops
		const Matrix3x3 Hr;   //
		inline ViewData() : view(*((const DepthData::ViewData*)this)) {}
		inline ViewData(const DepthData::ViewData& image0, const DepthData::ViewData& image1)
			: view(image1),
			Hl(image1.camera.K * image1.camera.R * image0.camera.R.t()),
			Hm(image1.camera.K * image1.camera.R * (image0.camera.C - image1.camera.C)),
			Hr(image0.camera.K.inv()) {}
	};

	CLISTDEF0(NeighborData) neighborsData; // neighbor pixel depths to be used for smoothing
	CLISTDEF0(ImageRef) neighbors; // neighbor pixels coordinates to be processed
	volatile Thread::safe_t& idxPixel; // current image index to be processed
	Vec3 X0;	      //
	ImageRef lt0;	  // constants during one pixel loop
	float normSq0;	  //
	TexelVec texels0; //
	TexelVec texels1;
	FloatArr scores;
	DepthMap& depthMap0;
	NormalMap& normalMap0;
	ConfidenceMap& confMap0;

	const CLISTDEF0(ViewData) images; // neighbor images used
	const DepthData::ViewData& image0;
	const Image32F& image0Sum; // integral image used to fast compute patch mean intensity
	const MapRefArr& coords;
	const Image8U::Size size;
	const IDX idxScore;
	const ENDIRECTION dir;
	const Depth dMin, dMax;

	DepthEstimator(DepthData& _depthData0, volatile Thread::safe_t& _idx, const Image32F& _image0Sum, const MapRefArr& _coords, ENDIRECTION _dir);

	bool PreparePixelPatch(const ImageRef&);
	bool FillPixelPatch(const ImageRef&);
	float ScorePixel(Depth, const Normal&);
	void ProcessPixel(IDX idx);
	
	inline float GetImage0Sum(const ImageRef& p0) {
		const ImageRef p1(p0.x+nSizeWindow, p0.y);
		const ImageRef p2(p0.x, p0.y+nSizeWindow);
		const ImageRef p3(p0.x+nSizeWindow, p0.y+nSizeWindow);
		return image0Sum(p3) - image0Sum(p2) - image0Sum(p1) + image0Sum(p0);
	}

	inline Matrix3x3f ComputeHomographyMatrix(const ViewData& img, Depth depth, const Normal& normal) const {
		#if 0
		// compute homography matrix
		const Matrix3x3f H(img.view.camera.K*HomographyMatrixComposition(image0.camera, img.view.camera, Vec3(normal), Vec3(X0*depth))*image0.camera.K.inv());
		#else
		// compute homography matrix as above, caching some constants
		const Vec3 n(normal);
		return (img.Hl + img.Hm * (n.t()*INVERT(n.dot(X0)*depth))) * img.Hr;
		#endif
	}

	static inline CLISTDEF0(ViewData) InitImages(const DepthData& depthData) {
		CLISTDEF0(ViewData) images(0, depthData.images.GetSize()-1);
		const DepthData::ViewData& image0(depthData.images.First());
		for (IDX i=1; i<depthData.images.GetSize(); ++i)
			images.AddConstruct(image0, depthData.images[i]);
		return images;
	}

	static inline Point3 ComputeRelativeC(const DepthData& depthData) {
		return depthData.images[1].camera.R*(depthData.images[0].camera.C-depthData.images[1].camera.C);
	}
	static inline Matrix3x3 ComputeRelativeR(const DepthData& depthData) {
		RMatrix R;
		ComputeRelativeRotation(depthData.images[0].camera.R, depthData.images[1].camera.R, R);
		return R;
	}

	// generate random depth and normal
	static inline Depth RandomDepth(Depth dMin, Depth dMax) {
		ASSERT(dMin > 0);
		return randomRange(dMin, dMax);
	}
	static inline Normal RandomNormal() {
		const float a1Min = FD2R(0.f);
		const float a1Max = FD2R(360.f);
		const float a2Min = FD2R(120.f);
		const float a2Max = FD2R(180.f);
		Normal normal;
		Dir2Normal(Point2f(randomRange(a1Min,a1Max), randomRange(a2Min,a2Max)), normal);
		ASSERT(normal.z < 0);
		return normal;
	}

	// encode/decode NCC score and refinement level in one float
	static inline float EncodeScoreScale(float score, unsigned invScaleRange=0) {
		ASSERT(score >= 0.f && score <= 2.01f);
		return score*0.1f+(float)invScaleRange;
	}
	static inline unsigned DecodeScale(float score) {
		return (unsigned)FLOOR2INT(score);
	}
	static inline unsigned DecodeScoreScale(float& score) {
		const unsigned invScaleRange(DecodeScale(score));
		score = (score-(float)invScaleRange)*10.f;
		//ASSERT(score >= 0.f && score <= 2.01f); //problems in multi-threading
		return invScaleRange;
	}
	static inline float DecodeScore(float score) {
		DecodeScoreScale(score);
		return score;
	}

	// Encodes/decodes a normalized 3D vector in two parameters for the direction
	template<typename T, typename TR>
	static inline void Normal2Dir(const TPoint3<T>& d, TPoint2<TR>& p) {
		// empirically tested
		ASSERT(ISEQUAL(norm(d), T(1)));
		p.y = TR(atan2(sqrt(d.x*d.x + d.y*d.y), d.z));
		p.x = TR(atan2(d.y, d.x));
	}
	template<typename T, typename TR>
	static inline void Dir2Normal(const TPoint2<T>& p, TPoint3<TR>& d) {
		// empirically tested
		const T siny(sin(p.y));
		d.x = TR(cos(p.x)*siny);
		d.y = TR(sin(p.x)*siny);
		d.z = TR(cos(p.y));
		ASSERT(ISEQUAL(norm(d), TR(1)));
	}

	static void MapMatrix2ZigzagIdx(const Image8U::Size& size, DepthEstimator::MapRefArr& coords, BitMatrix& mask, int rawStride=16);

	const float smoothBonusDepth, smoothBonusNormal;
	const float smoothSigmaDepth, smoothSigmaNormal;
	const float thMagnitudeSq;
	const float angle1Range, angle2Range;
	const float thConfSmall, thConfBig, thConfIgnore;
	static const float scaleRanges[12];
};
/*----------------------------------------------------------------*/


// Tools
unsigned EstimatePlane(const Point3Arr&, Plane&, double& maxThreshold, bool arrInliers[]=NULL, size_t maxIters=0);
unsigned EstimatePlaneLockFirstPoint(const Point3Arr&, Plane&, double& maxThreshold, bool arrInliers[]=NULL, size_t maxIters=0);
unsigned EstimatePlaneTh(const Point3Arr&, Plane&, double maxThreshold, bool arrInliers[]=NULL, size_t maxIters=0);
unsigned EstimatePlaneThLockFirstPoint(const Point3Arr&, Plane&, double maxThreshold, bool arrInliers[]=NULL, size_t maxIters=0);

void EstimatePointColors(const ImageArr& images, PointCloud& pointcloud);
void EstimatePointNormals(const ImageArr& images, PointCloud& pointcloud, int numNeighbors=16/*K-nearest neighbors*/);

bool SaveDepthMap(const String& fileName, const DepthMap& depthMap);
bool LoadDepthMap(const String& fileName, DepthMap& depthMap);
bool SaveNormalMap(const String& fileName, const NormalMap& normalMap);
bool LoadNormalMap(const String& fileName, NormalMap& normalMap);
bool SaveConfidenceMap(const String& fileName, const ConfidenceMap& confMap);
bool LoadConfidenceMap(const String& fileName, ConfidenceMap& confMap);

bool ExportDepthMap(const String& fileName, const DepthMap& depthMap, Depth minDepth=FLT_MAX, Depth maxDepth=0);
bool ExportNormalMap(const String& fileName, const NormalMap& normalMap);
bool ExportConfidenceMap(const String& fileName, const ConfidenceMap& confMap);
bool ExportPointCloud(const String& fileName, const Image&, const DepthMap&, const NormalMap&);
/*----------------------------------------------------------------*/

} // namespace MVS

#endif // _MVS_DEPTHMAP_H_
