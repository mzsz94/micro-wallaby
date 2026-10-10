import type { Metadata } from "next";
import ImuDashboard from "./ImuDashboard";
import "./imu.css";

export const metadata: Metadata = {
  title: "NU Motion Lab · IMU Dashboard",
  description: "NU40-DK·NU-87 IMU의 가속도, 각속도, 추정 기울기를 실시간으로 확인하는 로컬 센서 대시보드",
};

export default function ImuPage() {
  return <ImuDashboard />;
}
