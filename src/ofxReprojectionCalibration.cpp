#include "ofxReprojectionCalibration.h"

ofxReprojectionCalibration::ofxReprojectionCalibration(ofxBase3DVideo *cam, ofWindow projector_win, ofxReprojectionCalibrationConfig *config = NULL) {

	if(cam == NULL) {
		ofxLogWarning("ofxReprojection") << "Valid ofxBase3DVideo providing both color and depth image must be passed to constructor ofxReprojectionCalibration";
	} else {
		self->cam = cam;
	}


	if(config != NULL) {
		self->config = *config;
	}
}

ofxReprojectionCalibration::~ofxReprojectionCalibration() {
}

static void ofxReprojectionCalibration::lm_evaluate_camera_matrix(const double *par, int m_dat, const void *data, double *fvec, int *info) {
	
	// Calculate Ax = b for each 3d measurement x

	// Matrix A
	cv::Mat A;
	cv::Mat inputmatrix = cv::Mat(2,4,CV_64F, (void*)par);
	cv::vconcat(inputmatrix,lm_affinerow,A);

	vector<void*> *datasets = (vector<void*>*)data;

	void* data_kinect_voidp    = (*datasets)[0];
	void* data_projector_voidp = (*datasets)[1];
	
	vector< cv::Point3f > *data_kinect       = (vector< cv::Point3f >*)data_kinect_voidp;
	vector< cv::Point2f > *data_projector    = (vector< cv::Point2f >*)data_projector_voidp;

	for(int i = 0; i < m_dat/3; i++) { // for each measurement
		cv::Point3f x_data = (*data_kinect)[i];
		cv::Mat x = (cv::Mat_<double>(4,1) << x_data.x, x_data.y, x_data.z, 1);

		cv::Mat b = A*x;

		cv::Point2f y_data = (*data_projector)[i];
		cv::Mat y = (cv::Mat_<double>(3,1) << y_data.x, y_data.y, 1);

		// Reprojection error in each dimension.
		fvec[3*i + 0] = b.at<double>(0,0) - y.at<double>(0,0);
		fvec[3*i + 1] = b.at<double>(1,0) - y.at<double>(1,0);
		fvec[3*i + 2] = b.at<double>(2,0) - y.at<double>(2,0);
	}
}



static void ofxReprojectionCalibration::makechessboard(uchar* pixels, int img_width, int img_height, int rows, int cols, int x, int y, int width, int height, char brightness) {
	uchar bg = brightness;
	uchar black = 0;
	uchar white = brightness;

	for(int i = 0; i < img_width; i++) {
		for(int j = 0; j < img_height; j++) {
			uchar color;
			if( i < x or i >= x+width or j < y or j >= y+height) {
				color = bg;
			} else {
				int nx = (i-x)*(cols+1)/width;
				int ny = (j-y)*(rows+1)/height;

				int s = pow(-1,nx+ny);

				if( s > 0 ) {
					color = black;
				} else {
					color = white;
				}
			}

			pixels[i + j*img_width ] = color;
		}
	}
}

static ofxReprojectionCalibrationData loadDataFromFile(string filename) {
	string filename;
	if(arg_filename.empty()) {
		ofFileDialogResult filedialog = ofSystemLoadDialog("Load calibration measurements.",false,"~");
		cout << "opening " << filedialog.getPath() << endl;
		filename = filedialog.getPath();
	}  else {
		filename = arg_filename;
	}
	
	cv::FileStorage fs(filename, cv::FileStorage::READ);
	string timestamp; fs["timestamp"] >> timestamp;

	cout << "Loading measurements from timestamp " << timestamp << endl;

	int stored_kinect_width, stored_kinect_height;
	fs["kinect_width"] >> stored_kinect_width;
	fs["kinect_height"] >> stored_kinect_height;

	if(kinect_height != stored_kinect_height  ||  kinect_width != stored_kinect_width) {
		cout << "Error: stored measurements dimensions do not match Kinect input." << endl;
	}

	int stored_projector_width, stored_projector_height;
	fs["projector_width"] >> stored_projector_width;
	fs["projector_height"] >> stored_projector_height;

	if(projector_height != stored_projector_height  ||  projector_width != stored_projector_width) {
		cout << "Error: stored measurements dimensions do not match projector input." << endl;
	}

	fs["ref_max_depth"] >> ref_max_depth;
	kinect.ref_max_depth = ref_max_depth;

	cv::FileNode fn_measurements  = fs["measurements"];
	cv::FileNodeIterator it = fn_measurements.begin();

	vector< vector<cv::Point3f> > measurements; {
		for(int i = 0; it != fn_measurements.end(); it++, i++) {
			vector<cv::Point3f> measurement;

			cv::FileNodeIterator it2 = (*it).begin();
			for(int j = 0; it2 != (*it).end(); it2++, j++) {
				vector<double> point;
				(*it2) >> point;
				cv::Point3f p = cv::Point3f(point[0], point[1], point[2]);
				measurement.push_back(p);
				//cout << "measurement " << p << endl;
			}
			measurements.push_back(measurement);
		}
	}

	vector< vector<cv::Point2f> > projpoints; {
		cv::FileNode fn_projpoints  = fs["projpoints"];
		cv::FileNodeIterator it = fn_projpoints.begin();

		for(int i = 0; it != fn_projpoints.end(); it++, i++) {
			vector<cv::Point2f> projpoint;

			cv::FileNodeIterator it2 = (*it).begin();
			for(int j = 0; it2 != (*it).end(); it2++, j++) {
				vector<double> point;
				(*it2) >> point;
				cv::Point2f p = cv::Point2f(point[0], point[1]);
				projpoint.push_back(p);
			}
			projpoints.push_back(projpoint);
		}
	}

	fs.release();


}

static ofMatrix4x4 calculateReprojectionTransform(ofxReprojectionCalibrationData data) {

	// Put all measured points in one vector.
	//
	vector<cv::Point3f> measurements_all; {
		for(uint i  = 0; i < measurements.size(); i++) {
			for(uint j = 0; j < measurements[i].size(); j++) {
				measurements_all.push_back(measurements[i][j]);
			}
		}
	}
	vector<cv::Point2f> projpoints_all; {
		for(uint i  = 0; i < projpoints.size(); i++) {
			for(uint j = 0; j < projpoints[i].size(); j++) {
				projpoints_all.push_back(projpoints[i][j]);
			}
		}
	}

	// Transform measurement coodinates into world coordinates.
	//
	for(uint i = 0; i < measurements_all.size(); i++) {
		measurements_all[i] = kinect.pixel3f_to_world3f(measurements_all[i]);
	}
	for(uint i = 0; i < projpoints_all.size(); i++) {
		// Transform projector coordinates? (not necessary)
	}


	// Try calculating full 4x4 (affine) projection/camera matrix 
	// by fitting the data to a matrix by LM least squares (lmmin.c).
	// Two last rows should be (0,0,0,0; 0,0,0,1), so find the
	// unknown 2x4 matrix.

	vector<void*> lm_cam_data;

	lm_cam_data.push_back((void*) &(measurements_all)); 
	lm_cam_data.push_back((void*) &(projpoints_all)); 

	lm_status_struct lm_cam_status;
	lm_control_struct lm_cam_control = lm_control_double;
	lm_cam_control.printflags = 3;

	int n_par = 2*4;

	lmmin(n_par, lm_cam_params, measurements_all.size()*3, (const void*)&lm_cam_data, 
		lm_evaluate_camera_matrix,
		&lm_cam_control, &lm_cam_status, NULL);

	// Copy to openFrameworks matrix type.
	ofprojmat.set(
		       	lm_cam_params[0], lm_cam_params[1], lm_cam_params[2], lm_cam_params[3],
		       	lm_cam_params[4], lm_cam_params[5], lm_cam_params[6], lm_cam_params[7],
			0,0,0,0,
		       	0,0,0,1
	);


	ofidentitymat.set(1,0,0,0,   0,1,0,0,   0,0,1,0,    0,0,0,1);
	cout << "set identitymat to " << endl << ofidentitymat << endl;


	// Calculate reprojection error.
	double cum_err_2 = 0;
	for(uint i = 0; i < measurements_all.size(); i++) {
		ofVec3f v(
				measurements_all[i].x,
				measurements_all[i].y,
				measurements_all[i].z
			);
		cv::Point2f projpoint = projpoints_all[i];
		ofVec3f u = ofprojmat*v;
		double error = sqrt(pow(u.x-projpoint.x,2)+pow(u.y-projpoint.y,2));


		// Print all measured values with reprojection, for debugging.
		cout << measurements_all[i] << " " << projpoint << " " << u << " " << error << " " <<  endl;

		cum_err_2 += error*error;
	}
	double rms = sqrt(cum_err_2/ measurements_all.size() );


	cout << "Calculated transformation:" << endl;
	cout << ofprojmat << endl;
	cout << "Calculated RMS reprojection error: " << rms << endl;

}

void ofxReprojectionCalibration::update() {
	cam->update();
	if(cam->isFrameNew()) {

		// Convert color image to OpenCV image.
		unsigned char *pPixelsUC = (unsigned char*) cam->getPixels();
		cv::Mat chessdetectimage(cam_h,cam_w,CV_8UC(3), pPixelsUC); 

		vector<cv::Point2f> chesscorners;

		cv::Mat gray;
		cv::cvtColor(chessdetectimage, gray, CV_BGR2GRAY);

		// // Draw image with equialized histogram for inspection purposes.
		// // This image seems to be worse for detecting the chess board. 
		// // But it is sometimes recommended as an option to 
		// // findChessboardCorners (cv::CALIB_CB_NORMALIZE_IMAGE)
		// cv::Mat gray2;
		// cv::equalizeHist(gray,gray2);
		// color_image_debug.setFromPixels((unsigned char*) gray2.data, kinect_width, kinect_height, OF_IMAGE_GRAYSCALE);

		chessfound = false;

		if(measurement_pause and (ofGetSystemTime() - measurement_pause_time > MEASUREMENT_PAUSE_LENGTH)) {
			measurement_pause = false;
		}

		if(!measurement_pause) {
			chessfound =  cv::findChessboardCorners(gray, cv::Size(chess_cols,chess_rows), chesscorners,
				cv::CALIB_CB_ADAPTIVE_THRESH);// + cv::CALIB_CB_FAST_CHECK);
		}

		vector<cv::Point3f> chesscorners_depth;

		if(chessfound) {
			cv::cornerSubPix(gray, chesscorners, cv::Size(5, 5), cv::Size(-1, -1),
				cv::TermCriteria(CV_TERMCRIT_EPS + CV_TERMCRIT_ITER, 30, 0.1));

			// Add depth data to corners found (interpolate integer z coord to match fractional x,y coords)

			// Calculate matrix elements for solving planar regression (multiple linear regression)
			// to find R^2-value of plane (how planar is the chess board?).
			// 
			// This test is not strictly needed, but in case you are using a flat board to 
			// move around for the chessboard to be projected on, then this will be a test
			// that can give an indication of whether the measurements are good.
			// Can be disabled in config.use_planar_condition.

			double sumX = 0;
			double sumY = 0;
			double sumZ = 0;
			double sumX2 = 0;
			double sumY2 = 0;
			double sumXY = 0;
			double sumYZ = 0;
			double sumXZ = 0;
			double n = 0;
			
			chessfound_includes_depth = true;
			for(uint i = 0; i < chesscorners.size(); i++) {
				float *pDPixel = (float*) cam->getDistancePixels();

				// Calculate 3D point from color and depth image ("world coords")
				cv::Point3f p;
				p.x = chesscorners[i].x;
				p.y = chesscorners[i].y;

				int imgx1 = ((int) p.x);
				int imgx2 = ((int) p.x) +1;

				int imgy1 = ((int) p.y);
				int imgy2 = ((int) p.y) +1;

				// Check that all relevant depth values are valid;
				vector<int> depth_values_test;
				depth_values_test.push_back(imgx1+imgy1*cam_w);
				depth_values_test.push_back(imgx1+imgy2*cam_w);
				depth_values_test.push_back(imgx2+imgy1*cam_w);
				depth_values_test.push_back(imgx2+imgy2*cam_w);

				for(uint j = 0; j < depth_values_test.size(); j++) {
					int value = (int)pDPixel[depth_values_test[j]];
					if(value < DEPTH_MIN || value > DEPTH_MAX) {
						chessfound_includes_depth = false;
						break;
					}
				}



				float interp_x1, interp_x2, interp_z;

				// Bilinear interpolation to find z in depth map from fractional coords.
				// (The detected corners have sub-pixel precision.)
				interp_x1  = (imgx2-(float)p.x)/((float)(imgx2-imgx1))*((float)pDPixel[imgx1+imgy1*cam_w]);
				interp_x1 += ((float)p.x-imgx1)/((float)(imgx2-imgx1))*((float)pDPixel[imgx2+imgy1*cam_w]);

				interp_x2  = (imgx2-(float)p.x)/((float)(imgx2-imgx1))*((float)pDPixel[imgx2+imgy1*cam_w]);
				interp_x2 += ((float)p.x-imgx1)/((float)(imgx2-imgx1))*((float)pDPixel[imgx2+imgy2*cam_w]);

				interp_z   = (imgy2-(float)p.y)/((float)(imgy2-imgy1)) * interp_x1;
				interp_z  += ((float)p.y-imgy1)/((float)(imgy2-imgy1)) * interp_x2;

				p.z = interp_z;

				// Sum values for planar regression below
				sumX += p.x;
				sumY += p.y;
				sumZ += p.z;
				sumX2 += p.x*p.x;
				sumY2 += p.y*p.y;
				sumXY += p.x*p.y;
				sumYZ += p.y*p.z;
				sumXZ += p.x*p.z;
				n += 1;

				//cout << "interpolating depth value. close int: " << pDPixel[imgx1+imgy1*kinect_width]
				     //<< ", interp: " << p.z << endl;

				chesscorners_depth.push_back(p);
			}


			if(chessfound_includes_depth) {
				//cout << chesscorners_depth << endl;

				// Solve least squares to find plane equation
				cv::Mat lsq_left = ( cv::Mat_<double>(3,3)  << sumX2, sumXY, sumX, sumXY, sumY2, sumY, sumX, sumY, n );
				//cout << "lsq_left: " << lsq_left << endl;

				cv::Mat lsq_right = ( cv::Mat_<double>(3,1) << sumXZ, sumYZ, sumZ );

				cv::Mat_<double> plane;

				bool planesolved = cv::solve(lsq_left, lsq_right, plane);

				plane_r2 = 0;
				chessfound_planar = false;
				if(planesolved) {

					// Find R^2 of regression to assess planarity

					double ssres = 0;
					double sstot = 0;
					for(uint i = 0; i < chesscorners_depth.size(); i++) {
						double tot = chesscorners_depth[i].z-sumZ/n;
						sstot += tot*tot;

						double fz = plane(0,0)*chesscorners_depth[i].x 
							  + plane(0,1)*chesscorners_depth[i].y 
							  + plane(0,2);
						double res = chesscorners_depth[i].z - fz;
						ssres += res*res;
					}

					plane_r2 = 1 - ssres/sstot;

					if(config.use_planar_condition) { 
						chessfound_planar = plane_r2 > PLANAR_THRESHOLD;
					} else {
						chessfound_planar = true;
					}
				}
			}

			cv::drawChessboardCorners(chessdetectimage, cv::Size(chess_cols,chess_rows), cv::Mat(chesscorners), chessfound);
			color_image.setFromPixels(pPixelsUC, kinect_width, kinect_height, OF_IMAGE_COLOR);
		}

		// If chessboard is found, depth data exists and planarity check is satisfied, 
		// add this measurement to the stability buffer corner_history.

		bool frame_ok = chessfound && chessfound_includes_depth && chessfound_planar;
		if(!frame_ok) chesscorners_depth.clear();

		stability_buffer_i = (stability_buffer_i + 1)%(corner_history.size());
		corner_history[stability_buffer_i] = chesscorners_depth;

		// Count number of acceptable frames in stability buffer corner_history.

		chessfound_enough_frames = false;
		if(frame_ok) {
			num_ok_frames = 0;
			for(uint i = 0; i < corner_history.size(); i++) {
				if(corner_history[i].size() == chesscorners_depth.size()) {
					num_ok_frames += 1;
				}
			}
			if(num_ok_frames == corner_history.size()) {
				chessfound_enough_frames = true;
			}
		}

		// If enough consecutive acceptable frames/measurements have been found,
		// check variance within the stability buffer corner_history.

		chessfound_variance_ok = false;
		if(chessfound_enough_frames) {
			largest_variance_xy = 0;
			largest_variance_z  = 0;
			for(uint i = 0; i < corner_history[0].size(); i++) {
				for(uint j = 0; j < 3; j++) {
					double mean = 0;
					double variance = 0;

					for(uint k = 0; k < corner_history.size(); k++) {
						cv::Vec<float, 3> corner_history_vector = corner_history[k][i];
						mean += corner_history_vector[j];
					}
					mean /= corner_history.size();

					for(uint k = 0; k < corner_history.size(); k++) {
						cv::Vec<float, 3> corner_history_vector = corner_history[k][i];
						double dist = corner_history_vector[j] - mean;
						if(j == 0 or j == 1) {
							variance += dist*dist;
						} else if ( j== 2) {
							variance += (dist*dist) / corner_history_vector[j];
						}
					}
					variance /= corner_history.size();

					//cout << "variance for corner #" << i << " dimension " << j << " = " << variance << endl;


					if( (j == 0 or j == 1) and variance > largest_variance_xy) {
						largest_variance_xy = variance;
					}

					if( j == 2 and variance > largest_variance_z) {
						largest_variance_z = variance;
					}
				}
			}

			if(largest_variance_xy < VARIANCE_THRESHOLD_XY and largest_variance_z < VARIANCE_THRESHOLD_Z) {
				chessfound_variance_ok = true;

				// Measurement is accepted. Calculate the mean and
				// add to valid_measurements and all_chessboard_points.

				vector<cv::Point3f> measurement_mean;
				for(uint i = 0; i < corner_history[0].size(); i++) {
					cv::Vec<float, 3> corner;
					for(uint j= 0 ; j < 3; j ++) {
						double mean = 0; 
						for(uint k = 0; k < corner_history.size(); k++) {
							cv::Vec<float, 3> corner_history_vector = corner_history[k][i];
							mean += corner_history_vector[j];
						}
						mean /= corner_history.size();
						corner[j] = mean;
					}
					
					cv::Point3f cornerp = corner;
					measurement_mean.push_back(cornerp);
				}

				ofxLogVerbose("ofxReprojection") << "Measurement: " << cv::Mat(measurement_mean) << endl;


				valid_measurements.push_back(measurement_mean);


				vector<cv::Point2f> chessboard_points;
				for(int y = 0; y < chess_rows; y++) {
					for(int x = 0; x < chess_cols; x++) {
						int px = chess_x + ((int)((x+1)*chess_width/(chess_cols+1)));
						int py = chess_y + ((int)((y+1)*chess_height/(chess_rows+1)));
						cv::Point2f p((float)px,(float)py);

						ofxLogVerbose("ofxReprojection") << "chessboard point " << p << endl;
						chessboard_points.push_back(p);
					}
				}

				all_chessboard_points.push_back(chessboard_points);

				
				measurement_pause = true;
				measurement_pause_time = ofGetSystemTime();
			}
		}
	}
}
void ofReprojectionCalibration::drawCalibrationStatusScreen(){

	projector_chessboard.draw(1920,0);

	kinect.color_image.draw(0,0);
	kinect.depth_image.draw(kinect_width,0);

	ofCircle(kinect_width/2, kinect_height/2, 5);

	color_image_debug.draw(kinect_width,kinect_height);

	string str = "framerate is ";                       
	str += ofToString(ofGetFrameRate(), 2)+"fps"; 
	
	ofDrawBitmapString(str, 20,kinect_height+20);

	ostringstream msg; msg << "Valid measurements: " << valid_measurements.size();
	ofDrawBitmapString(msg.str(), 20, 2*kinect_height-20);

	ostringstream msg2; msg2 << "Press 'd' to drop last measurement, 'c' to clear, 's' to save and exit." << endl;
	ofDrawBitmapString(msg2.str(), 20, 2*kinect_height-40);

	ostringstream msg3; msg3 << "Planar threshold " << PLANAR_THRESHOLD << ", variance threshold XY " << VARIANCE_THRESHOLD_XY
					<< " Z " << VARIANCE_THRESHOLD_Z << "." << endl;
	ofDrawBitmapString(msg3.str(), 20, 2*kinect_height-60);

	ofColor c_error(200,0,0);
	ofColor c_success(0,200,0);
	ofColor c_white(255,255,255);

	if(measurement_pause) {
		ofSetColor(c_white);
		ofDrawBitmapString("Pausing before next measurement...", 20, kinect_height+40);
	} else {
		if(!chessfound) {
			ofSetColor(c_error);
			ofDrawBitmapString("Chess board not detected.",20, kinect_height+40);
		} else {
			ofSetColor(c_success);
			ofDrawBitmapString("Chess board detected.",20, kinect_height+40);

			if(!chessfound_includes_depth) {
				ofSetColor(c_error);
				ofDrawBitmapString("Depth data for chess board is incomplete.", 20, kinect_height+60);
			} else {
				ofSetColor(c_success);
				ofDrawBitmapString("Depth data complete.", 20, kinect_height+60);

				if(!chessfound_planar) {
					ofSetColor(c_error);
					ostringstream msg; msg << "Chessboard is not planar (R^2 = " << plane_r2 << ").";
					ofDrawBitmapString(msg.str(), 20, kinect_height+80);
				} else {
					ofSetColor(c_success);
					ostringstream msg; msg << "Chessboard is planar (R^2 = " << plane_r2 << ").";
					ofDrawBitmapString(msg.str(), 20, kinect_height+80);

					if(!chessfound_enough_frames) {
						ofSetColor(c_error);
						ostringstream msg; msg << "Values for " << num_ok_frames 
							<< "/" << NUM_STABILITY_FRAMES << " frames";
						ofDrawBitmapString(msg.str(), 20, kinect_height+100);
					} else {
						ofSetColor(c_success);
						ostringstream msg; msg << "Values for " << num_ok_frames 
							<< "/" << NUM_STABILITY_FRAMES << " frames";
						ofDrawBitmapString(msg.str(), 20, kinect_height+100);

						if(!chessfound_variance_ok) {
							ofSetColor(c_error);
							ostringstream msg; msg << "Variance too high (xy " << largest_variance_xy 
								<< " (" << VARIANCE_THRESHOLD_XY << "), z " << largest_variance_z
								<< " (" << VARIANCE_THRESHOLD_Z << ")).";
							ofDrawBitmapString(msg.str(), 20, kinect_height+120);
						} else {
							ofSetColor(c_success);
							ostringstream msg; msg << "Variance OK (xy " << largest_variance_xy 
								<< " (" << VARIANCE_THRESHOLD_XY << "), z " << largest_variance_z
								<< " (" << VARIANCE_THRESHOLD_Z << ")).";
							ofDrawBitmapString(msg.str(), 20, kinect_height+120);
						}
					}
				}
			}
		}
	}

	ofSetColor(255,255,255);
		



}

static void saveDataToFile(ofxReprojectionCalibrationData data, string filename) {

	//ofFileDialogResult filename = ofSystemSaveDialog(ofGetTimestampString(),"Save measurements.");

	cv::FileStorage fs(filename.getPath(), cv::FileStorage::WRITE);

	fs << "timestamp" << ofGetTimestampString();

	fs << "kinect_width" << kinect_width;
	fs << "kinect_height" << kinect_height;

	fs << "projector_width" << projector_width;
	fs << "projector_height" << projector_height;

	fs << "ref_max_depth" << kinect.ref_max_depth;

	fs << "measurements" << "[";

	for(uint i = 0; i < valid_measurements.size() ; i++) {
		fs << "[";
		for(uint j = 0; j < valid_measurements[i].size(); j++) {
			fs << valid_measurements[i][j];
		}
		fs << "]";

	} 

	fs << "]";

	fs << "projpoints" << "[";

	for(uint i = 0; i < all_chessboard_points.size() ; i++) {
		fs << "[";
		for(uint j = 0; j < all_chessboard_points[i].size(); j++) {
			fs << all_chessboard_points[i][j];
		}
		fs << "]";

	} 

	fs << "]";

	fs.release();
}


