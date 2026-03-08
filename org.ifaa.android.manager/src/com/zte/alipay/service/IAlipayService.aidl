package com.zte.alipay.service;

interface IAlipayService {
    byte[] startCmd(in byte[] param);
    int[] getIdList();
}
