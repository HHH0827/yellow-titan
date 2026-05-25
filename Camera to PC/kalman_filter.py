from dataclasses import dataclass


@dataclass
class ScalarKalmanFilter:
    """One-dimensional Kalman filter for smoothing noisy scalar measurements."""

    process_noise: float            #Q  系统本身变化的不确定性，Q 变大，更相信新变化，响应更快
    measurement_noise: float        #R  摄像头测量的不确定，R 变小，更相信当前测量值，响应更快
    estimate: float | None = None   #  当前滤波后的估计值
    covariance: float = 1.0         #P 当前估计得不确定性

    def update(self, measurement: float) -> float:  #第一次没有历史数据，直接把第一帧的测量值当成初始估计
        if self.estimate is None:
            self.estimate = measurement   #当前帧测出来的原始颜色比例
            return self.estimate

        self.covariance += self.process_noise     #P = P + Q    如果Q大说明真实颜色变化可能很快
        kalman_gain = self.covariance / (self.covariance + self.measurement_noise)  #当前测量值的权重  K = P/(P+R)
        self.estimate += kalman_gain * (measurement - self.estimate)   #新的测量值  new = old + k *（当前测量值和旧估计值之间的误差）
        self.covariance = (1.0 - kalman_gain) * self.covariance   #更新P，如果P很大说明用了很多新的测量值，更新后不确定性会下降
        return self.estimate

