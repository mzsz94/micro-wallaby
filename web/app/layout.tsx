import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "NU Play Lab",
  description: "부모가 설계하고 승인한 오늘의 놀이를 NU40-DK 센서 리모컨으로 즐기는 로컬 플레이 플랫폼",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="ko">
      <body>{children}</body>
    </html>
  );
}
