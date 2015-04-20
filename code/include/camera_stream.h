/**
 * @file    camera_stream.h
 * @author	Michael Baxter	<20503664@student.uwa.edu.au>
 * @author  Jeremy Tan <20933708@student.uwa.edu.au>
 * 
 * Class used to start camera stream.
 * 
 * Wonderful omni-function camera streaming action fun!
 * 
 **/
 
#ifndef _PICOPTERX_CAMERA_STREAM_H
#define _PICOPTERX_CAMERA_STREAM_H

#include "common.h"
#include "cv.h"
#include "highgui.h" //For CvCapture
//#include "opencv2/imgproc/imgproc.hpp"

#define STREAM_FILE "/home/pi/out.mjpg"

#define LOOKUP_SIZE 8
#define CHAR_SIZE 256

#define CAMERA_OK 0

#define CAMERA_WIDTH 320
#define CAMERA_HEIGHT 240
 
namespace picopter {
    /* Forward declaration of the options class */
    class Options;
    
    typedef struct {
        int x;
        int y;
    } ObjectLocation;

    typedef struct {
        int x;
        int y;
        int l;
        int w;
    } CamWindow;

    class CameraStream {
        public:
            typedef enum {
                MODE_NO_PROCESSING = 0,
                MODE_COM = 1,
                MODE_CAMSHIFT = 2,
                MODE_CONNECTED_COMPONENTS = 3
            } CameraMode;
            
            CameraStream();
            CameraStream(Options *opts);
            virtual ~CameraStream(void);
            
            bool Start(void);
            void Stop(void);
            
            CameraMode GetMode(void);
            void SetMode(CameraMode mode);
            
            bool ObjectDetected(void);
            int NumObjectsDetected(void);
            void GetObjectLocations(std::vector<ObjectLocation>*);
            
            double GetFramerate(void);
            void TakePhoto(std::string);
        private:
            int MIN_HUE, MAX_HUE, MIN_SAT, MAX_SAT, MIN_VAL, MAX_VAL, PIXEL_THRESHOLD;
            double PROCESS_IMAGE_REDUCE, STREAM_IMAGE_REDUCE;
            int DILATE_ELEMENT, ERODE_ELEMENT;
            int PIXEL_SKIP;
            int THREAD_SLEEP_TIME;
            double BOX_SIZE;
            
            std::atomic<bool> m_stop;
            CameraMode m_mode;
            
            std::mutex m_worker_mutex;
            std::future<void> m_worker_thread;
            
            CvCapture* m_capture;
            
            time_t start_time, end_time;
            int frame_counter;
            double m_fps;
            
            bool m_save_photo;
            std::string m_save_filename;
            
            uchar lookup_threshold[LOOKUP_SIZE][LOOKUP_SIZE][LOOKUP_SIZE];
            uchar lookup_reduce_colourspace[CHAR_SIZE];
            
            void ProcessImages(void);
            bool centerOfMass(cv::Mat& Isrc);
            bool camShift(cv::Mat& Isrc);
            int connectComponents(cv::Mat& Isrc);
            
            std::vector<ObjectLocation> redObjectList;
            std::vector<CamWindow> windowList;

            std::vector<cv::Scalar> windowColours;


            void RGB2HSV(int r, int g, int b, int *h, int *s, int *v);
            void build_lookup_threshold(uchar lookup_threshold[][LOOKUP_SIZE][LOOKUP_SIZE], int minHue, int maxHue, int minSat, int maxSat, int minVal, int maxVal);
            void build_lookup_reduce_colourspace(uchar lookup_reduce_colourspace[]);
            int unreduce(int x);
            
            void drawCrosshair(cv::Mat img);
            void drawObjectMarker(cv::Mat img, cv::Point centre, cv::Scalar colour);
            void drawBox(cv::Mat img, cv::Point topLeft, cv::Point bottomRight, cv::Scalar colour);
            void drawFramerate(cv::Mat img);
            
            void buildColours(std::vector<cv::Scalar>*);
            
            /** Copy constructor (disabled) **/
            CameraStream(const CameraStream &other);
            /** Assignment operator (disabled) **/
            CameraStream& operator= (const CameraStream &other);
    };
}

#endif // _PICOPTERX_CAMERA_STREAM_H