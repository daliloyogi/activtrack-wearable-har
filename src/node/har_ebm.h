/*
 * har_ebm.h -- Human Activity Recognition on embedded targets.
 *
 * Pure C99, no dynamic allocation, no libm dependency in the default build.
 * Two stages, usable independently:
 *
 *   1. har_extract_features()  20 raw accel+gyro samples -> 30 features
 *   2. har_ebm_predict()       30 features -> 4 class scores (log-odds)
 *
 * The model is an ExplainableBoostingClassifier exported from
 * models/ebm_model.pkl by export_cpp.py. Inference is a per-feature table
 * lookup plus an add, so it needs no multiplies and no math library.
 */
#ifndef HAR_EBM_H
#define HAR_EBM_H

#ifdef __cplusplus
extern "C"
{
#endif

#define HAR_WINDOW_SIZE 20
#define HAR_N_FEATURES 30
#define HAR_N_CLASSES 4

    /* Class indices of the model, matching models/labels.txt (training data recorded with
     * the old app order: Standing = 0, Sitting = 1). The current app codes Sitting = 0,
     * Standing = 1; the node maps model classes to app codes with EBM_ACT_CODE. */
    enum
    {
        HAR_STANDING = 0,
        HAR_SITTING = 1,
        HAR_WALKING = 2,
        HAR_RUNNING = 3
    };

    extern const char *const har_labels[HAR_N_CLASSES];

/*
 * Accumulator type for feature extraction. Defaults to double, which
 * reproduces the Python pipeline bit for bit. Define HAR_ACC_FLOAT on a
 * target without a double-precision FPU; see README.md for the error this
 * introduces.
 */
#ifdef HAR_ACC_FLOAT
    typedef float har_acc_t;
#else
typedef double har_acc_t;
#endif

    /*
     * One window of sensor data, oldest sample first. Accelerometer in g,
     * gyroscope in deg/s -- the units the node1 IMU reports, and the units the
     * current model was trained on. Feeding m/s^2 or rad/s gives wrong classes.
     * Keep this as a ring buffer on the device and pass it in place.
     */
    typedef struct
    {
        float accel_x[HAR_WINDOW_SIZE];
        float accel_y[HAR_WINDOW_SIZE];
        float accel_z[HAR_WINDOW_SIZE];
        float gyro_x[HAR_WINDOW_SIZE];
        float gyro_y[HAR_WINDOW_SIZE];
        float gyro_z[HAR_WINDOW_SIZE];
    } har_window_t;

    /* Feature order expected by the model; index with these for readability. */
    enum
    {
        HAR_F_ACCEL_STD = 0,
        HAR_F_ACCEL_MEAN,
        HAR_F_ACCEL_MAX,
        HAR_F_ACCEL_MIN,
        HAR_F_ACCEL_RANGE,
        HAR_F_ACCEL_RMS,
        HAR_F_ACCEL_ENERGY,
        HAR_F_GYRO_STD,
        HAR_F_GYRO_MEAN,
        HAR_F_GYRO_MAX,
        HAR_F_GYRO_MIN,
        HAR_F_GYRO_RANGE,
        HAR_F_GYRO_RMS,
        HAR_F_GYRO_ENERGY,
        HAR_F_ACCELX_MEAN,
        HAR_F_ACCELY_MEAN,
        HAR_F_ACCELZ_MEAN,
        HAR_F_ACCELX_STD,
        HAR_F_ACCELY_STD,
        HAR_F_ACCELZ_STD,
        HAR_F_GYROX_MEAN,
        HAR_F_GYROY_MEAN,
        HAR_F_GYROZ_MEAN,
        HAR_F_GYROX_STD,
        HAR_F_GYROY_STD,
        HAR_F_GYROZ_STD,
        HAR_F_STANDING_INDICATOR,
        HAR_F_AXIS_GAP_1,
        HAR_F_AXIS_GAP_2,
        HAR_F_AXIS_GAP_RATIO
    };

    extern const char *const har_feature_names[HAR_N_FEATURES];

    /* Fills out[HAR_N_FEATURES] from one window. */
    void har_extract_features(const har_window_t *w, float out[HAR_N_FEATURES]);

    /* Fills scores[HAR_N_CLASSES] with additive log-odds (higher = more likely). */
    void har_ebm_predict(const float features[HAR_N_FEATURES],
                         float scores[HAR_N_CLASSES]);

    /* Index of the largest score; ties resolve to the lowest index. */
    int har_ebm_argmax(const float scores[HAR_N_CLASSES]);

    /* Window -> class index, the common path. */
    int har_classify(const har_window_t *w);

/*
 * Optional softmax, for when you want calibrated probabilities rather than a
 * label. Pulls in expf() from libm, so it is compiled out by default.
 */
#ifdef HAR_WANT_PROBABILITIES
    void har_ebm_softmax(const float scores[HAR_N_CLASSES],
                         float probabilities[HAR_N_CLASSES]);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HAR_EBM_H */
